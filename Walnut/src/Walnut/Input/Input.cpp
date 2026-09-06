#include "Input.h"

#include "Walnut/Application.h"

#include <SDL3/SDL.h>

namespace Walnut {

	bool Input::IsKeyDown(KeyCode keycode)
	{
		const bool* state = SDL_GetKeyboardState(nullptr);
		return state[(int)keycode];
	}

	bool Input::IsMouseButtonDown(MouseButton button)
	{
		const SDL_MouseButtonFlags flags = SDL_GetMouseState(nullptr, nullptr);
		return (flags & SDL_BUTTON_MASK((uint32_t)button + 1)) != 0;
	}

	glm::vec2 Input::GetMousePosition()
	{
		float x, y;
		SDL_GetMouseState(&x, &y);
		return { x, y };
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		SDL_Window* windowHandle = Application::Get().GetWindowHandle();
		switch (mode)
		{
			case CursorMode::Normal:
				SDL_SetWindowRelativeMouseMode(windowHandle, false);
				SDL_ShowCursor();
				break;
			case CursorMode::Hidden:
				SDL_HideCursor();
				break;
			case CursorMode::Locked:
				SDL_SetWindowRelativeMouseMode(windowHandle, true);
				break;
		}
	}

}