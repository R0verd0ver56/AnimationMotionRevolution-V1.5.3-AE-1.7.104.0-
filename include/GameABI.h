#pragma once

// Minimal game ABI helpers for the vertical-lift physics hooks.
namespace amr::abi
{
	static_assert(offsetof(RE::bhkCharacterController, context) == 0x1E0);
	static_assert(offsetof(RE::bhkCharacterController, wantState) == 0x21C);

	inline RE::hkpCharacterState* RegisteredState(RE::hkpCharacterContext* a_context, RE::hkpCharacterStateType a_type)
	{
		const auto index = static_cast<std::uint32_t>(a_type);
		return a_context && a_context->stateManager && index < RE::hkpCharacterStateType::kTotal ?
				   a_context->stateManager->registeredState[index] :
				   nullptr;
	}

	// Calls the state's SimulateStatePhysics through its vtable (slot 8), so every hook in the chain runs.
	inline void SimulateStatePhysics(RE::bhkCharacterState* a_state, RE::bhkCharacterController* a_controller)
	{
		if (!a_state || !a_controller) {
			return;
		}
		using function_type = void (*)(RE::bhkCharacterState*, RE::bhkCharacterController*);
		const auto vtable = *reinterpret_cast<const function_type* const*>(a_state);
		vtable[0x08](a_state, a_controller);
	}
}
