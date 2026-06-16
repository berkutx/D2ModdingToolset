/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2023 Vladimir Makeev.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "generationresultinterf.h"
#include "autodialog.h"
#include "button.h"
#include "dialoginterf.h"
#include "image2memory.h"
#include "mempool.h"
#include "menubase.h"
#include "menurandomscenario.h"
#include "multilayerimg.h"
#include "pictureinterf.h"

namespace hooks {

static void __fastcall acceptButtonHandler(CGenerationResultInterf* thisptr, int /* %edx */)
{
    thisptr->onAccept(thisptr->menu);
}

static void __fastcall rejectButtonHandler(CGenerationResultInterf* thisptr, int /* %edx */)
{
    thisptr->onReject(thisptr->menu);
}

static void __fastcall cancelButtonHandler(CGenerationResultInterf* thisptr, int /* %edx */)
{
    thisptr->onCancel(thisptr->menu);
}

static void __fastcall copyButtonHandler(CGenerationResultInterf* thisptr, int /*%edx*/)
{
    if (thisptr && thisptr->previewImage) {
        copyImageToClipboard(thisptr->previewImage);
    }
}

static int pixelIndex(int x, int y, int size, float scaleFactor = 1.0f)
{
    const int i = static_cast<int>(x / scaleFactor);
    const int j = static_cast<int>(y / scaleFactor);

    return i + size * j;
}

static void drawChar(char ch,
                     rsg::Position topLeft,
                     int mapWidth,
                     int mapHeight,
                     std::vector<game::Color>& tileColoring)
{
    using namespace game;

    // Pixel font 5x7 [0-9][A-Z]
    static const uint8_t font[36][7] = {
        // 0-9 (indexes 0-9)
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
        {0x1F, 0x01, 0x01, 0x02, 0x04, 0x08, 0x10}, // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9

        // A-Z (indexes 10-35)
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
        {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
        {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, // D
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}, // G
        {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
        {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
        {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
        {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
        {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
        {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}, // S
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
        {0x11, 0x11, 0x11, 0x11, 0x15, 0x15, 0x0A}, // W
        {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
        {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, // Y
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}  // Z
    };

    int index = -1;
    if (ch >= '0' && ch <= '9')
        index = ch - '0';
    else if (ch >= 'A' && ch <= 'Z')
        index = ch - 'A' + 10;
    else
        return;

    const uint8_t* rows = font[index];
    const Color fg(255, 255, 255, 255); // white
    const Color outline(0, 0, 0, 255);  // black

    // 7x9
    for (int dy = 0; dy < 9; ++dy) {
        for (int dx = 0; dx < 7; ++dx) {
            rsg::Position p(topLeft.x + dx, topLeft.y + dy);
            if (p.x < 0 || p.x >= mapWidth || p.y < 0 || p.y >= mapHeight)
                continue;

            int idx = pixelIndex(p.x, p.y, mapWidth);
            // 5x7
            bool isSymbol = (dx >= 1 && dx <= 5 && dy >= 1 && dy <= 7
                             && (rows[dy - 1] & (0x10 >> (dx - 1))));

            if (isSymbol) {
                tileColoring[idx] = fg;
            } else {
                bool isOutline = false;
                for (int ny = dy - 1; ny <= dy + 1; ++ny) {
                    for (int nx = dx - 1; nx <= dx + 1; ++nx) {
                        if (nx >= 1 && nx <= 5 && ny >= 1 && ny <= 7) {
                            if (rows[ny - 1] & (0x10 >> (nx - 1))) {
                                isOutline = true;
                                break;
                            }
                        }
                    }
                    if (isOutline)
                        break;
                }
                if (isOutline) {
                    tileColoring[idx] = outline;
                }
            }
        }
    }
}

static CImage2Memory* createPreviewImage(CMenuRandomScenario* menu)
{
    using namespace game;

    const int size = menu->scenario->size;
    const int width = size;
    const int height = size;
    const int previewSize = 144;
    const float scaleFactor = static_cast<float>(previewSize) / static_cast<float>(size);

    CImage2Memory* preview = createImage2Memory(previewSize, previewSize);

    rsg::MapGenerator* generator{menu->generator.get()};
    // Each pixel represents map tile that is colored according to its zone
    std::vector<Color> tileColoring(width * height);
    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            rsg::Position pos{i, j};
            const int index = pixelIndex(i, j, width);
            const int zoneId = generator->zoneColoring[generator->posToIndex(pos)];
            const int colorIndex = zoneId % 100;

            static const Color colors[] = {
                Color{255, 0, 0, 255},     // Zone id 0: red
                Color{0, 255, 0, 255},     // 1: green
                Color{0, 0, 255, 255},     // 2: blue
                Color{255, 255, 255, 255}, // 3: white
                Color{0, 0, 0, 255},       // 4: black
                Color{127, 127, 127, 255}, // 5: gray
                Color{255, 255, 0, 255},   // 6: yellow
                Color{0, 255, 255, 255},   // 7: cyan
                Color{255, 0, 255, 255},   // 8: magenta
                Color{255, 153, 0, 255},   // 9: orange
                Color{0, 158, 10, 255},    // 10: dark green
                Color{0, 57, 158, 255},    // 11: dark blue
                Color{158, 57, 0, 255},    // 12: dark red
            };

            const int colorsTotal = static_cast<int>(std::size(colors));
            if (colorIndex < colorsTotal) {
                tileColoring[index] = colors[colorIndex];
            } else {
                const uint8_t c = 32 + (colorIndex - colorsTotal) * 2;
                tileColoring[index] = Color(c, c, c, 255);
            }
        }
    }

    for (const auto& [id, zone] : generator->zones) {
        char label = zone->getLabel();
        rsg::Position topLeft(zone->getPosition().x - 3, zone->getPosition().y - 4);
        drawChar(label, topLeft, width, height, tileColoring);
    }

    // Scale tile coloring up to 144 x 144 pixels.
    // It is better to show preview images of the same size regardless of scenario size
    for (int i = 0; i < previewSize; ++i) {
        for (int j = 0; j < previewSize; ++j) {
            const int sourceIndex{pixelIndex(i, j, size, scaleFactor)};
            const int destIndex{pixelIndex(i, j, previewSize)};

            preview->pixels[destIndex] = tileColoring[sourceIndex];
        }
    }

    return preview;
}

CGenerationResultInterf* createGenerationResultInterf(CMenuRandomScenario* menu,
                                                      OnGenerationResultAccepted onAccept,
                                                      OnGenerationResultRejected onReject,
                                                      OnGenerationResultCanceled onCancel)
{
    using namespace game;

    static const char dialogName[] = "DLG_GENERATION_RESULT";

    const auto& allocateMemory = Memory::get().allocate;

    CImage2Memory* previewImage = nullptr;

    auto popup = (CGenerationResultInterf*)allocateMemory(sizeof(CGenerationResultInterf));
    CPopupDialogInterfApi::get().constructor(popup, dialogName, nullptr);

    popup->onAccept = onAccept;
    popup->onReject = onReject;
    popup->onCancel = onCancel;
    popup->menu = menu;

    CDialogInterf* dialog{*popup->dialog};

    using ButtonCallback = CMenuBaseApi::Api::ButtonCallback;
    const auto& dialogApi{CDialogInterfApi::get()};

    const auto& createButtonFunctor = CMenuBaseApi::get().createButtonFunctor;
    const auto& assignFunctor = CButtonInterfApi::get().assignFunctor;
    const auto& smartPtrFree = SmartPointerApi::get().createOrFreeNoDtor;

    SmartPointer functor;

    {
        auto callback = (ButtonCallback)acceptButtonHandler;
        createButtonFunctor(&functor, 0, (CMenuBase*)popup, &callback);
        assignFunctor(dialog, "BTN_ACCEPT", dialogName, &functor, 0);
        smartPtrFree(&functor, nullptr);
    }

    {
        auto callback = (ButtonCallback)rejectButtonHandler;
        createButtonFunctor(&functor, 0, (CMenuBase*)popup, &callback);
        assignFunctor(dialog, "BTN_REJECT", dialogName, &functor, 0);
        smartPtrFree(&functor, nullptr);
    }

    {
        auto callback = (ButtonCallback)cancelButtonHandler;
        createButtonFunctor(&functor, 0, (CMenuBase*)popup, &callback);
        assignFunctor(dialog, "BTN_CANCEL", dialogName, &functor, 0);
        smartPtrFree(&functor, nullptr);
    }

    static const char btnCopyName[]{"BTN_COPY"};
    if (dialogApi.findControl(dialog, btnCopyName)) {
        auto callback = (ButtonCallback)copyButtonHandler;
        createButtonFunctor(&functor, 0, (CMenuBase*)popup, &callback);
        assignFunctor(dialog, btnCopyName, dialogName, &functor, 0);
        smartPtrFree(&functor, nullptr);
    }

    {
        IMqImage2* border = AutoDialogApi::get().loadImage("ZONES_BORDER");
        CImage2Memory* preview = createPreviewImage(menu);
        popup->previewImage = preview;

        std::vector<uint8_t> pngData;
        hooks::writeImageToMemory(preview, pngData);
        menu->scenario->setPreviewImage(pngData);

        CMultiLayerImg* image{(CMultiLayerImg*)allocateMemory(sizeof(CMultiLayerImg))};

        const auto& multilayerApi{CMultiLayerImgApi::get()};
        multilayerApi.constructor(image);

        // Make sure preview is inside border image
        const int borderOffset = 7;

        multilayerApi.addImage(image, border, -999, -999);
        multilayerApi.addImage(image, preview, borderOffset, borderOffset);

        CPictureInterf* zonesImage = CDialogInterfApi::get().findPicture(dialog, "IMG_ZONES");

        CMqPoint offset{};
        CPictureInterfApi::get().setImage(zonesImage, image, &offset);
    }

    return popup;
}

} // namespace hooks
