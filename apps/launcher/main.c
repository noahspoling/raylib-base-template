#include "app.h"
#include "native_modules.h"

int main(int argc, char **argv) {
    AppConfig cfg = {
        .mode         = APP_LAUNCHER,
        .single       = false,
        .start_scene  = "splash",
        .argc         = argc,
        .argv         = argv,
        .native       = GAME_NATIVE_MODULES,
        .native_count = GAME_NATIVE_MODULE_COUNT,
    };
    return gramarye_app_run(&cfg);
}
