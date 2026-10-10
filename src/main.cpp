#include <raylib.h>

int main(){
InitWindow(1000, 800, "Hello, World!");

    while (!WindowShouldClose()){
    BeginDrawing();
        ClearBackground(BLACK);
        DrawText("Hello, World!", 500, 400, 30, WHITE);
    EndDrawing();
    }

CloseWindow();
}
