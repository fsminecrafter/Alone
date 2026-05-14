#include <nds.h>
#include <stdio.h>

int main()
{
    consoleDemoInit();
    consoleClear();
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);

    vramSetPrimaryBanks(
        VRAM_A_MAIN_BG,
        VRAM_B_MAIN_SPRITE,
        VRAM_C_SUB_BG,
        VRAM_D_SUB_SPRITE
    );

    PrintConsole topScreen, bottomScreen;

    consoleInit(&topScreen,    3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true,  true);
    consoleInit(&bottomScreen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    consoleSelect(&topScreen);
    printf("TOP SCREEN\n");

    consoleSelect(&bottomScreen);
    printf("BOTTOM SCREEN\n");

    while (1)
    {
        swiWaitForVBlank();

        scanKeys();
        int keys = keysDown();

        if (keys & KEY_START)
        {
            consoleSelect(&bottomScreen);
            printf("Start pressed!\n");
        }
    }
}
