#pragma once
// Animated title screen. Models are software-rendered into turntable sprite sheets once at startup,
// afterwards every frame is just a handful of textured quads.

void titleInit();      // call once after InitWindow
void titleDraw();      // call every frame while on the welcome screen (background + logo)
void titleShutdown();  // call before CloseWindow

// Menu helpers: titleMenuBegin(); if (titleMenuButton("Enter")) ...; titleMenuButton("Multiplayer"); titleMenuEnd();
void titleMenuBegin();
bool titleMenuButton(const char *label);
void titleMenuEnd();
