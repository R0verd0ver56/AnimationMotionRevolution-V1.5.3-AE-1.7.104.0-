#pragma once

// Vertical root motion (ported onto AMR 1.5.4.5 for Skyrim 1.6.1170 / 1.7.104).
//
// The game projects animation motion onto the ground plane while a character is in the on-ground
// state, so the Z part of an `animmotion` curve (launchers such as Vergil's Rising Star / Rising Sun
// or Dante's power attack, ~220-250 units up) never lifts the actor. This drives the height directly:
//
//   * the translation hook records, per character controller AND per clip, the curve's height and
//     speed, and the actor's height when the clip started;
//   * the physics hooks steer the controller to "start height + curve height" (curve speed plus a
//     correction for any shortfall), switching it to in-air when a launch starts;
//   * the curve is followed while it rises and while it holds at the top (Vergil hovers there);
//     once it comes down more than kReleaseDrop below its highest point, the actor is released to
//     normal gravity / CGAT so landing is detected normally.
//
// Per-clip tracking matters: Open Animation Replacer keeps a duplicate of each replaced clip
// generator ("Duplicate_MCO_ClipGenerator_...") and the game can sample both in the same frame.

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <xmmintrin.h>

#include "RE/Skyrim.h"

namespace amr::vertical
{
	using Clock = std::chrono::steady_clock;

	// A sample older than this no longer counts (the translation hook refreshes it every frame).
	inline constexpr auto kSampleLifetime = std::chrono::milliseconds(100);
	// Game units per Havok unit.
	inline constexpr float kHavokToGame = 69.991249F;
	// A clip only counts as a launch once its curve rises faster than this (game units / s). Real launches
	// rise at 500-2000 u/s; small values are curve noise and must not pop the actor off the ground.
	inline constexpr float kMinLiftSpeed = 150.0F;
	// v6: ...and only once it has also risen at least this far above where it started (game units). Real
	// launches rise 150-250 units; small bumps in ordinary attacks must never switch anyone to in-air.
	inline constexpr float kMinRise = 20.0F;
	// Release the actor to gravity once the curve has come down this far below its highest point.
	inline constexpr float kReleaseDrop = 15.0F;
	// How strongly a height shortfall / overshoot is corrected (per second).
	inline constexpr float kCorrection = 12.0F;
	// Vertical speed limit (game units / s).
	inline constexpr float kMaxSpeed = 3000.0F;

	inline float Z(const RE::hkVector4& a_vector)
	{
		alignas(16) float f[4];
		_mm_store_ps(f, a_vector.quad);
		return f[2];
	}

	inline void SetZ(RE::hkVector4& a_vector, float a_z)
	{
		alignas(16) float f[4];
		_mm_store_ps(f, a_vector.quad);
		f[2] = a_z;
		a_vector.quad = _mm_load_ps(f);
	}

	// Height of the character controller in game units.
	inline float ControllerHeight(RE::bhkCharacterController* a_controller)
	{
		RE::hkVector4 position;
		a_controller->GetPosition(position, true);
		return Z(position) * kHavokToGame;
	}

	struct ClipTrack
	{
		Clock::time_point stamp{};  // last sample
		float             prevTime{ 0.0F };
		float             prevZ{ 0.0F };   // curve height at prevTime
		float             speed{ 0.0F };   // curve speed, game units / s, + up
		float             baseZ{ 0.0F };   // actor height where the curve's 0 is
		float             maxZ{ 0.0F };    // highest curve height so far
		float             startZ{ 0.0F };  // curve height at the clip's first sample
		bool              hasSpeed{ false };
		bool              rose{ false };    // curve rose faster than kMinLiftSpeed: this is a launch
		bool              released{ false };  // curve came down: gravity / CGAT have the actor now
	};

	struct ActorTrack
	{
		std::unordered_map<const void*, ClipTrack> clips;
		Clock::time_point                           lastLog{};
	};

	struct Registry
	{
		std::mutex                                                     lock;
		std::unordered_map<const RE::hkpCharacterContext*, ActorTrack> actors;
	};

	inline Registry& Get()
	{
		static Registry registry;
		return registry;
	}

	// v6: lifts are only for the player, the player's followers / summons and copies of the player
	// (Vergil's Doppelganger). Enemies' attacks are never turned into launches.
	inline bool LiftAllowed(RE::Character* a_character)
	{
		if (!a_character) {
			return false;
		}
		if (a_character->IsPlayerRef() || a_character->IsPlayerTeammate() || a_character->IsCommandedActor()) {
			return true;
		}
		const auto* base = a_character->GetActorBase();
		return base && base->GetFormID() == 0x7;
	}

	// Translation hook, custom (animmotion) branch: a_translation is the cumulative displacement of
	// the clip at a_motionTime.
	inline void Sample(RE::Character* a_character, const void* a_clip, float a_motionTime, const RE::NiPoint3& a_translation,
		bool a_clipHasVerticalMotion)
	{
		if (!a_clipHasVerticalMotion || !a_character || !LiftAllowed(a_character)) {
			return;  // other clips never cancel a running curve; samples simply expire
		}
		auto* controller = a_character->GetCharController();
		if (!controller) {
			return;
		}
		const RE::hkpCharacterContext* context = std::addressof(controller->context);
		const float                    actorZ = ControllerHeight(controller);

		auto& r = Get();
		std::lock_guard guard(r.lock);
		auto&       t = r.actors[context].clips[a_clip];
		const auto  now = Clock::now();
		const float dt = a_motionTime - t.prevTime;
		if (t.stamp == Clock::time_point{} || now - t.stamp > kSampleLifetime || dt < -0.25F) {
			// first sample of this clip (or it restarted)
			t = ClipTrack{};
			t.baseZ = actorZ - a_translation.z;
			t.maxZ = a_translation.z;
			t.startZ = a_translation.z;
		} else if (dt < 0.0005F) {
			// The game samples a clip more than once per frame (previous and current time): keep the speed.
			t.stamp = now;
			return;
		} else {
			t.speed = (a_translation.z - t.prevZ) / dt;
			t.hasSpeed = true;
			if (!t.rose) {
				if (t.speed > kMinLiftSpeed && a_translation.z - t.startZ >= kMinRise) {
					t.rose = true;
				} else {
					// not launched yet: keep the curve's 0 at the actor's current height (slopes, steps)
					t.baseZ = actorZ - a_translation.z;
				}
			}
			t.maxZ = std::max(t.maxZ, a_translation.z);
			if (t.rose && a_translation.z < t.maxZ - kReleaseDrop) {
				t.released = true;
			}
		}
		t.prevTime = a_motionTime;
		t.prevZ = a_translation.z;
		t.stamp = now;
	}

	// Physics hooks: vertical velocity (game units / s) that keeps the actor on the curve right now.
	// Returns false when no launch curve is being followed for this actor.
	inline bool Command(const RE::hkpCharacterContext* a_context, float a_currentZ, float& a_velocity, float& a_targetZ,
		bool& a_logNow)
	{
		auto& r = Get();
		std::lock_guard guard(r.lock);
		const auto it = r.actors.find(a_context);
		if (it == r.actors.end()) {
			return false;
		}
		const auto       now = Clock::now();
		const ClipTrack* best = nullptr;
		for (auto c = it->second.clips.begin(); c != it->second.clips.end();) {
			if (now - c->second.stamp > kSampleLifetime) {
				c = it->second.clips.erase(c);
				continue;
			}
			if (c->second.hasSpeed && c->second.rose && !c->second.released && (!best || c->second.stamp > best->stamp)) {
				best = std::addressof(c->second);
			}
			++c;
		}
		if (it->second.clips.empty()) {
			r.actors.erase(it);
			return false;
		}
		if (!best) {
			return false;
		}
		a_targetZ = best->baseZ + best->prevZ;
		a_velocity = std::clamp(best->speed + kCorrection * (a_targetZ - a_currentZ), -kMaxSpeed, kMaxSpeed);
		a_logNow = now - it->second.lastLog > std::chrono::milliseconds(100);
		if (a_logNow) {
			it->second.lastLog = now;
		}
		return true;
	}

	// Kept for the vanilla-motion branch of the translation hook: no longer cancels anything (see Sample).
	inline void Clear(RE::Character*) {}

	inline void ClearAll()
	{
		auto& r = Get();
		std::lock_guard guard(r.lock);
		r.actors.clear();
	}
}
