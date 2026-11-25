#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// ============================================================================
// Cute Hermit Crab Beach Screensaver
// ============================================================================

constexpr int16_t CRAB_SCR_W = 320;
constexpr int16_t CRAB_SCR_H = 240;
constexpr int16_t BAND_Y = 96;      // Top of active animation band (y = 96..239)
constexpr int16_t BAND_H = 144;     // 320x144x8-bit sprite = ~46 KB

static TFT_eSprite crabSpr(&tft);
static bool crabSprReady = false;

enum CrabState
{
    CRAB_WALKING,
    CRAB_HIDING_FULL,
    CRAB_PEEKING
};

static CrabState crabState = CRAB_WALKING;
static unsigned long stateStartedMs = 0;
static float crabX = 160.0f;
static int crabDir = 1; // 1 = right, -1 = left
static float walkPhase = 0.0f;

static bool ensureCrabSprite()
{
    if (!crabSprReady)
    {
        crabSpr.setColorDepth(8);
        if (crabSpr.createSprite(CRAB_SCR_W, BAND_H) == nullptr)
        {
            Serial.println("[CRAB] Failed to allocate 320x144 sprite!");
            return false;
        }
        crabSprReady = true;
    }
    return true;
}

// Draw realistic moonlit tropical night sky above the active band (y = 0..95)
static void drawStaticBeachSky(int driftOffset)
{
    // Deep indigo-to-twilight vertical gradient
    tft.fillRect(0, 0, CRAB_SCR_W, 48, TFT_BLACK);
    tft.fillRect(0, 48, CRAB_SCR_W, 20, 0x0043); // Midnight blue
    tft.fillRect(0, 68, CRAB_SCR_W, 14, 0x0085); // Deep horizon blue
    tft.fillRect(0, 82, CRAB_SCR_W, 8, 0x0108);  // Twilight glow
    tft.fillRect(0, 90, CRAB_SCR_W, 6, 0x018B);  // Horizon mist

    // Soft glowing crescent moon on upper-right
    int moonX = 258 + (driftOffset / 2);
    int moonY = 30;
    tft.fillCircle(moonX, moonY, 15, 0x2126);     // Faint outer halo
    tft.fillCircle(moonX, moonY, 11, 0xFFDF);     // Warm pearl moon
    tft.fillCircle(moonX - 5, moonY - 3, 10, TFT_BLACK); // Crescent cutout

    // Subtle distant horizon clouds
    int cloudX = (65 + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
    tft.fillRoundRect(cloudX, 58, 44, 6, 3, 0x08A6);
    tft.fillRoundRect(cloudX + 10, 54, 24, 6, 3, 0x10E8);

    // Twinkling stars with slow burn-in drift
    const int starX[] = {24, 72, 116, 158, 198, 294, 44, 138, 218, 182, 92, 282};
    const int starY[] = {14, 30, 12, 24, 16, 18, 46, 40, 52, 60, 64, 62};
    const uint16_t starCol[] = {TFT_WHITE, 0xB7FF, 0xFFE8, TFT_SILVER};

    for (int i = 0; i < 12; i++)
    {
        int sx = (starX[i] + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
        int sy = starY[i];
        uint16_t col = starCol[i % 4];
        tft.drawPixel(sx, sy, col);
        if (i % 3 == 0)
        {
            tft.drawPixel(sx - 1, sy, 0x39E7);
            tft.drawPixel(sx + 1, sy, 0x39E7);
            tft.drawPixel(sx, sy - 1, 0x39E7);
            tft.drawPixel(sx, sy + 1, 0x39E7);
        }
    }
}

// Draw layered lagoon, rolling surf foam & textured tropical sand (local y = 0..143)
static void drawBeachBackground(TFT_eSprite &spr, float timeSec, int driftOffset)
{
    // 1. Layered tropical ocean strip (local y = 0..28)
    spr.fillRect(0, 0, CRAB_SCR_W, 10, 0x0149);  // Deep lagoon navy
    spr.fillRect(0, 10, CRAB_SCR_W, 9, 0x024E);  // Mid lagoon teal
    spr.fillRect(0, 19, CRAB_SCR_W, 8, 0x0393);  // Shallow luminous turquoise

    // Shimmering moonlight reflections on the water beneath the moon
    int moonRefX = 258 + (driftOffset / 2);
    for (int r = 0; r < 5; r++)
    {
        int shimW = 14 - (r * 2) + (int)(sinf(timeSec * 3.0f + r) * 3.0f);
        int shimX = moonRefX - (shimW / 2) + (int)(cosf(timeSec * 2.2f + r) * 2.0f);
        spr.drawFastHLine(shimX, 3 + (r * 4), max(2, shimW), 0xB7FF);
    }

    // 2. Gentle rolling wave & curved foam shoreline (local y = 26..33)
    int foamShift = (int)(sinf(timeSec * 1.5f) * 3.0f);
    int waveY = 26 + foamShift;

    // Wet darkened sand behind the receding wave
    spr.fillRect(0, 23, CRAB_SCR_W, 18, 0x8B25);
    spr.fillRect(0, waveY - 2, CRAB_SCR_W, 4, 0x04B5); // Wave crest
    spr.fillRect(0, waveY + 2, CRAB_SCR_W, 2, 0xE7FF); // Crisp white seafoam edge

    // Scalloped foam bubbles along the shoreline
    for (int fx = 0; fx < CRAB_SCR_W; fx += 14)
    {
        int bx = (fx + (driftOffset / 2) + CRAB_SCR_W) % CRAB_SCR_W;
        spr.fillCircle(bx, waveY + 3, 2, 0xB7FF);
    }

    // 3. Multi-tone warm golden sand (local y = 34..143)
    spr.fillRect(0, waveY + 6, CRAB_SCR_W, 10, 0xB447); // Damp transition sand
    spr.fillRect(0, waveY + 14, CRAB_SCR_W, BAND_H - (waveY + 14), 0xE5EB); // Sun-warmed golden sand
    spr.fillRect(0, 116, CRAB_SCR_W, 28, 0xCD09); // Soft foreground sand dune shadow

    // 4. Natural sand grain speckles & tiny shells/pebbles
    const uint16_t speckCols[] = {0xD549, 0xF6F2, 0xAC06, 0xBE48};
    for (int i = 0; i < 55; i++)
    {
        int px = ((i * 73) + driftOffset) % CRAB_SCR_W;
        if (px < 0) px += CRAB_SCR_W;
        int py = 46 + ((i * 29) % 94);
        spr.fillCircle(px, py, (i % 7 == 0) ? 2 : 1, speckCols[i % 4]);
    }

    // Decorative 5-arm starfish on the left beach
    int starSx = (52 + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
    int starSy = 124;
    for (int a = 0; a < 5; a++)
    {
        float ang = (a * 72.0f - 90.0f) * 0.0174533f;
        int tipX = starSx + (int)(cosf(ang) * 7.0f);
        int tipY = starSy + (int)(sinf(ang) * 5.0f);
        spr.drawLine(starSx, starSy, tipX, tipY, 0xE9A6);
        spr.drawLine(starSx + 1, starSy, tipX, tipY, 0xFA68);
    }
    spr.fillCircle(starSx, starSy, 2, 0xFB4A);

    // Tiny white scallop seashell on the right beach
    int shX = (268 + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
    int shY = 128;
    spr.fillCircle(shX, shY, 4, 0xFFDF);
    spr.drawLine(shX - 3, shY + 2, shX + 3, shY + 2, 0xD549);
}
// Draw the gorgeous spiral conch shell centered around (cx, cy)
// dir: 1 = facing right (shell spirals to left), -1 = facing left (shell spirals to right)
static void drawGorgeousShell(TFT_eSprite &spr, int cx, int cy, int dir, float tiltAngle)
{
    const uint16_t colOuter    = 0xFA68; // Warm coral-salmon
    const uint16_t colMid      = 0xFD4C; // Peachy sunset orange
    const uint16_t colCream    = 0xFF77; // Warm pearl cream
    const uint16_t colTip      = 0xE26B; // Rose-magenta apex
    const uint16_t colRim      = 0x60A1; // Deep warm brown-maroon outline

    int backDir = -dir;
    int bobY = (int)(sinf(tiltAngle) * 2.0f);

    // 1. Pointed spiral spire (back-most whorls)
    int spireX = cx + backDir * 30;
    int spireY = cy - 12 + bobY;
    spr.fillTriangle(
        spireX + backDir * 12, spireY - 4,
        spireX - backDir * 4,  spireY - 12,
        spireX - backDir * 2,  spireY + 6,
        colTip);
    spr.drawLine(spireX + backDir * 12, spireY - 4, spireX - backDir * 4, spireY - 12, colRim);
    spr.drawLine(spireX + backDir * 12, spireY - 4, spireX - backDir * 2, spireY + 6, colRim);

    // 2. Whorl 3 (rear spiral chamber)
    int w3x = cx + backDir * 21;
    int w3y = cy - 10 + bobY;
    spr.fillCircle(w3x, w3y, 12, colCream);
    spr.drawCircle(w3x, w3y, 12, colRim);
    spr.fillCircle(w3x - backDir * 2, w3y - 4, 4, TFT_WHITE);

    // 3. Whorl 2 (middle chamber)
    int w2x = cx + backDir * 11;
    int w2y = cy - 8 + bobY;
    spr.fillCircle(w2x, w2y, 16, colMid);
    spr.drawCircle(w2x, w2y, 16, colRim);
    spr.drawArc(w2x, w2y, 13, 11, 200, 340, colCream, colMid);
    spr.fillCircle(w2x - backDir * 3, w2y - 6, 4, TFT_WHITE);

    // 4. Main Body Whorl (large front chamber)
    int w1x = cx - backDir * 2;
    int w1y = cy - 5 + bobY;
    spr.fillCircle(w1x, w1y, 21, colOuter);
    spr.drawCircle(w1x, w1y, 21, colRim);
    spr.drawArc(w1x, w1y, 17, 15, 190, 350, colCream, colOuter);
    spr.drawArc(w1x, w1y, 12, 10, 200, 340, colMid, colOuter);
    spr.fillCircle(w1x - backDir * 4, w1y - 8, 5, 0xFFDF);
    spr.fillCircle(w1x - backDir * 9, w1y - 4, 2, TFT_WHITE);

    // 5. Dark hollow shell opening (aperture) facing forward
    int openX = cx + dir * 13;
    int openY = cy + 3 + bobY;
    spr.fillEllipse(openX, openY, 10, 14, 0x2820);
    spr.drawEllipse(openX, openY, 11, 15, colCream);
    spr.drawEllipse(openX, openY, 12, 16, colRim);
}

// Draw extra-cute cartoon eyes on stalks + bouncy V-tipped antennae
static void drawCrabEyesAndAntennae(TFT_eSprite &spr, int baseX, int baseY, int dir,
                                    float lookX, float lookY, bool blinking, float peekRatio)
{
    if (peekRatio <= 0.05f)
        return;

    const uint16_t colBody = 0xFA03; // Bright cheerful coral-orange
    const uint16_t colRim  = 0x60A1;

    int stalkH = (int)(14.0f * peekRatio);
    int eyeR = (peekRatio > 0.7f) ? 7 : 5; // Bigger, cuter cartoon eyes!

    // Slightly angled cute eye stalks
    int leftEyeX  = baseX + dir * 3 - 6;
    int rightEyeX = baseX + dir * 5 + 6;
    int eyeY      = baseY - stalkH;

    spr.drawLine(baseX + dir * 2 - 3, baseY + 2, leftEyeX, eyeY, colBody);
    spr.drawLine(baseX + dir * 2 - 2, baseY + 2, leftEyeX + 1, eyeY, colBody);
    spr.drawLine(baseX + dir * 4 + 3, baseY + 2, rightEyeX, eyeY, colBody);
    spr.drawLine(baseX + dir * 4 + 4, baseY + 2, rightEyeX + 1, eyeY, colBody);

    // Bouncy antennae with tiny ball tips
    int antTipX1 = leftEyeX - 8 - (int)(lookX * 1.5f);
    int antTipX2 = rightEyeX + 8 + (int)(lookX * 1.5f);
    int antTipY  = eyeY - 7;
    spr.drawLine(leftEyeX - 2, eyeY + 3, antTipX1, antTipY, colBody);
    spr.drawLine(rightEyeX + 2, eyeY + 3, antTipX2, antTipY, colBody);
    spr.fillCircle(antTipX1, antTipY, 1, 0xFC86);
    spr.fillCircle(antTipX2, antTipY, 1, 0xFC86);

    // Big expressive cartoon eyeballs with double catchlights
    int ex[] = {leftEyeX, rightEyeX};
    for (int i = 0; i < 2; i++)
    {
        if (blinking)
        {
            spr.fillCircle(ex[i], eyeY, eyeR, colBody);
            spr.drawCircle(ex[i], eyeY, eyeR, colRim);
            // Happy curved closed-eye lashes
            spr.drawArc(ex[i], eyeY - 1, eyeR - 2, eyeR - 3, 200, 340, colRim, colBody);
        }
        else
        {
            spr.fillCircle(ex[i], eyeY, eyeR, TFT_WHITE);
            spr.drawCircle(ex[i], eyeY, eyeR, colRim);

            int px = ex[i] + (int)constrain(lookX, -2.0f, 2.0f);
            int py = eyeY + (int)constrain(lookY, -2.0f, 2.0f);
            spr.fillCircle(px, py, 3, TFT_BLACK);
            spr.fillCircle(px - 1, py - 2, 1, TFT_WHITE); // Primary catchlight
            spr.drawPixel(px + 2, py + 1, TFT_WHITE);    // Secondary sparkle catchlight
        }
    }
}
// Vertical sand bounds inside the 144px beach sprite (y = 0 is water, y = 42..143 is sand)
static float crabY = 96.0f;
static float crabVy = 0.35f;
constexpr float SAND_MIN_Y = 66.0f;  // Just below the wet shoreline foam
constexpr float SAND_MAX_Y = 128.0f; // Near bottom edge of sand (leaves room for legs/shadow)

// Render a complete frame of the hermit crab in its current state
static void renderHermitCrabFrame(float timeSec, int driftOffset)
{
    if (!ensureCrabSprite())
        return;

    drawBeachBackground(crabSpr, timeSec, driftOffset);

    int cx = (int)roundf(crabX);
    int groundY = (int)roundf(crabY); // Dynamic sand contact line inside sprite

    // Soft ground shadow beneath the crab
    int shadowW = (crabState == CRAB_WALKING) ? 35 : 28;
    crabSpr.fillEllipse(cx, groundY + 4, shadowW, 5, 0x9B86);

    const uint16_t colCrab      = 0xFA03; // Bright cheerful orange-red
    const uint16_t colCrabLight = 0xFC86; // Warm claw/face highlight
    const uint16_t colCrabDark  = 0xB181; // Back leg shading
    const uint16_t colRim       = 0x60A1;

    if (crabState == CRAB_WALKING)
    {
        int bodyY = groundY - 12;

        // 1. Animated walking legs (4 visible legs scuttling in alternating pairs)
        for (int i = 0; i < 4; i++)
        {
            float legPhase = walkPhase + (i * 1.57f);
            int strideX = (int)(sinf(legPhase) * 5.0f);
            int liftY   = (int)(max(0.0f, -cosf(legPhase)) * 4.0f);

            int hipX  = cx + crabDir * (2 + i * 4) - crabDir * 6;
            int hipY  = bodyY + 4;
            int kneeX = hipX + crabDir * 5 + strideX / 2;
            int kneeY = hipY + 5 - liftY / 2;
            int footX = hipX + crabDir * 8 + strideX;
            int footY = groundY + 2 - liftY;

            uint16_t lCol = (i % 2 == 0) ? colCrab : colCrabDark;
            crabSpr.drawLine(hipX, hipY, kneeX, kneeY, lCol);
            crabSpr.drawLine(hipX + 1, hipY, kneeX + 1, kneeY, lCol);
            crabSpr.drawLine(kneeX, kneeY, footX, footY, lCol);
            crabSpr.drawLine(kneeX + 1, kneeY, footX + 1, footY, lCol);
        }

        // 2. Gorgeous spiral shell on its back
        drawGorgeousShell(crabSpr, cx - crabDir * 4, bodyY - 8, crabDir, walkPhase * 0.5f);

        // 3. Cute chubby crab head/face peeking out of the shell aperture
        int headX = cx + crabDir * 14;
        int headY = bodyY + 1;
        crabSpr.fillEllipse(headX, headY, 10, 8, colCrab);
        crabSpr.drawEllipse(headX, headY, 10, 8, colRim);

        // Rosy blush cheeks & sweet happy smile
        crabSpr.fillCircle(headX + crabDir * 5, headY + 2, 2, 0xFC94);
        crabSpr.drawPixel(headX + crabDir * 2, headY + 3, colRim);
        crabSpr.drawPixel(headX + crabDir * 3, headY + 4, colRim);
        crabSpr.drawPixel(headX + crabDir * 4, headY + 4, colRim);
        crabSpr.drawPixel(headX + crabDir * 5, headY + 3, colRim);

        // 4. Cute waving front pincer claw + tiny secondary claw
        int clawWave = (int)(sinf(walkPhase * 0.7f) * 2.5f);
        int bigClawX = headX + crabDir * 12;
        int bigClawY = headY + 2 + clawWave;
        crabSpr.fillEllipse(bigClawX, bigClawY, 8, 5, colCrabLight);
        crabSpr.drawEllipse(bigClawX, bigClawY, 8, 5, colRim);
        crabSpr.drawLine(bigClawX + crabDir * 2, bigClawY, bigClawX + crabDir * 8, bigClawY - 1, colRim);

        // Tiny lower feeder claw
        int smallClawX = headX + crabDir * 7;
        int smallClawY = headY + 7 - (clawWave / 2);
        crabSpr.fillCircle(smallClawX, smallClawY, 3, colCrabLight);
        crabSpr.drawCircle(smallClawX, smallClawY, 3, colRim);

        // 5. Bobbing eye stalks & blinking
        bool blink = ((int)(timeSec * 10.0f) % 37 == 0);
        drawCrabEyesAndAntennae(crabSpr, headX, headY - 5, crabDir, (float)crabDir * 1.5f, crabVy * 1.5f, blink, 1.0f);
    }
    else
    {
        // HIDING OR PEEKING STATE: Shell rests snug on the sand
        int shellY = groundY - 6;
        drawGorgeousShell(crabSpr, cx - crabDir * 2, shellY, crabDir, 0.0f);

        // Tucked-in orange claw tip blocking the shell entrance
        int plugX = cx + crabDir * 12;
        int plugY = shellY + 4;
        crabSpr.fillEllipse(plugX, plugY, 6, 4, colCrabLight);
        crabSpr.drawEllipse(plugX, plugY, 6, 4, colRim);

        if (crabState == CRAB_PEEKING)
        {
            unsigned long peekElapsed = millis() - stateStartedMs;
            float peekRatio = constrain((float)peekElapsed / 450.0f, 0.0f, 1.0f);

            // Nervous look-around: look left, look right, blink twice!
            float lookSeq = sinf((float)peekElapsed * 0.0045f) * 2.2f;
            bool blink1 = (peekElapsed > 650 && peekElapsed < 820);
            bool blink2 = (peekElapsed > 1850 && peekElapsed < 2020);

            drawCrabEyesAndAntennae(crabSpr, plugX - crabDir * 2, plugY - 3, crabDir,
                                    lookSeq, -0.5f, (blink1 || blink2), peekRatio);
        }
    }

    crabSpr.pushSprite(0, BAND_Y);
}

// Called when the user taps the screen while the screensaver is active.
// Returns true if the screensaver should EXIT (second tap while hiding/peeking),
// or false if the crab was startled into hiding (first tap).
bool handleScreenSaverTap()
{
    unsigned long now = millis();

    if (crabState == CRAB_WALKING)
    {
        crabState = CRAB_HIDING_FULL;
        stateStartedMs = now;
        return false; // Stay in screensaver
    }
    else
    {
        crabState = CRAB_WALKING;
        if (crabSprReady)
        {
            crabSpr.deleteSprite();
            crabSprReady = false;
        }
        return true; // Wake up and free sprite RAM!
    }
}

// Main screensaver update loop (keeps existing signature for google-ai-main.cpp)
void updateScreenSaverEyes(bool forceInit = false)
{
    static unsigned long lastFrameMs = 0;
    static int lastDriftOffset = 999;

    unsigned long now = millis();
    float timeSec = (float)now * 0.001f;

    // Slow horizontal burn-in drift for background stars & sand pebbles
    int driftOffset = (int)roundf(sinf(timeSec * 0.08f) * 14.0f);

    if (forceInit)
    {
        crabState = CRAB_WALKING;
        stateStartedMs = now;
        crabX = 160.0f;
        crabY = 96.0f;
        crabDir = 1;
        crabVy = 0.32f;
        walkPhase = 0.0f;
        lastDriftOffset = driftOffset;
        tft.fillRect(0, BAND_Y, CRAB_SCR_W, BAND_H, TFT_BLACK);
        drawStaticBeachSky(driftOffset);
        renderHermitCrabFrame(timeSec, driftOffset);
        lastFrameMs = now;
        return;
    }

    // Update sky stars only when drift offset shifts by a pixel
    if (driftOffset != lastDriftOffset)
    {
        lastDriftOffset = driftOffset;
        drawStaticBeachSky(driftOffset);
    }

    // Cap animation at ~25 FPS (40ms per frame)
    if (now - lastFrameMs < 40)
        return;
    lastFrameMs = now;

    unsigned long inStateMs = now - stateStartedMs;

    if (crabState == CRAB_WALKING)
    {
        crabX += (float)crabDir * 1.35f;
        crabY += crabVy;
        walkPhase += 0.28f;

        // Horizontal beach bounds
        if (crabX >= 262.0f)
        {
            crabX = 262.0f;
            crabDir = -1;
        }
        else if (crabX <= 58.0f)
        {
            crabX = 58.0f;
            crabDir = 1;
        }

        // Vertical sand bounds (gentle diagonal wander with slight speed variation on bounce)
        if (crabY >= SAND_MAX_Y)
        {
            crabY = SAND_MAX_Y;
            crabVy = -0.28f - ((now % 3) * 0.06f);
        }
        else if (crabY <= SAND_MIN_Y)
        {
            crabY = SAND_MIN_Y;
            crabVy = 0.28f + ((now % 3) * 0.06f);
        }
    }
    else if (crabState == CRAB_HIDING_FULL)
    {
        // Stay completely tucked inside shell for 1.2s, then start peeking
        if (inStateMs >= 1200UL)
        {
            crabState = CRAB_PEEKING;
            stateStartedMs = now;
        }
    }
    else if (crabState == CRAB_PEEKING)
    {
        // Peek and look around for 3.8s (1.2s hide + 3.8s peek = 5.0s total)
        if (inStateMs >= 3800UL)
        {
            crabState = CRAB_WALKING;
            stateStartedMs = now;
        }
    }

    renderHermitCrabFrame(timeSec, driftOffset);
}
