// The Vita's touch panels as buttons, posted by inputsystem/joystick_sdl.cpp.
// The rear touchpad's halves are the stick clicks the Vita lacks
// (KEY_XBUTTON_STICK1 and STICK2). The front touchscreen's halves are two
// joystick buttons SDL never posts on the Vita, "JOY11" and "JOY12" in
// binds; they only act in game, where the screen isn't the menus' mouse.
// Include after inputsystem/ButtonCode.h
#ifndef VITA_BUTTONS_H
#define VITA_BUTTONS_H

#define VITA_FRONT_TOUCH_LEFT	JOYSTICK_BUTTON( 0, 10 )	// JOY11
#define VITA_FRONT_TOUCH_RIGHT	JOYSTICK_BUTTON( 0, 11 )	// JOY12

#endif
