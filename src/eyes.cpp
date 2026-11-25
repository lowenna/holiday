#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// ============================================================================
// Cute Hermit Crab Beach Screensaver (Replaces Blinking Eyes)
// ============================================================================

constexpr int16_t CRAB_SCR_W = 320;
constexpr int16_t CRAB_SCR_H = 240;
constexpr int16_t BAND_Y = 96;      // Top of active animation band (y = 96..239)
constexpr int16_t BAND_H = 144;     // 320x144x16-bit sprite = ~92 KB

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
        crabSpr.setColorDepth(8); // 320x144x8-bit = ~46 KB (half the RAM of 16-bit)
        if (crabSpr.createSprite(CRAB_SCR_W, BAND_H) == nullptr)
        {
            Serial.println("[CRAB] Failed to allocate 320x144 sprite!");
            return false;
        }
        crabSprReady = true;
    }
    return true;
}

// Draw static night-beach sky + gentle stars above the active band (y = 0..95)
static void drawStaticBeachSky(int driftOffset)
{
    tft.fillRect(0, 0, CRAB_SCR_W, BAND_Y, TFT_BLACK);

    // Subtle deep-twilight horizon glow near y = 80..95
    tft.fillRect(0, 82, CRAB_SCR_W, 8, 0x0084);
    tft.fillRect(0, 90, CRAB_SCR_W, 6, 0x0107);

    // Twinkling stars with slow burn-in drift
    const int starX[] = {28, 74, 118, 165, 212, 258, 296, 48, 142, 238, 190, 95};
    const int starY[] = {16, 34, 14, 28, 18, 38, 20, 56, 48, 54, 66, 68};
    const uint16_t starCol[] = {TFT_WHITE, TFT_CYAN, TFT_YELLOW, TFT_SILVER};

    for (int i = 0; i < 12; i++)
    {
        int sx = (starX[i] + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
        int sy = starY[i];
        uint16_t col = starCol[i % 4];
        tft.drawPixel(sx, sy, col);
        if (i % 3 == 0)
        {
            tft.drawPixel(sx - 1, sy, TFT_DARKGREY);
            tft.drawPixel(sx + 1, sy, TFT_DARKGREY);
            tft.drawPixel(sx, sy - 1, TFT_DARKGREY);
            tft.drawPixel(sx, sy + 1, TFT_DARKGREY);
        }
    }
}

// Draw beach water edge & warm golden sand inside the sprite (local y = 0..143)
static void drawBeachBackground(TFT_eSprite &spr, float timeSec, int driftOffset)
{
    // 1. Calm moonlit ocean strip at top of band (local y = 0..26)
    spr.fillRect(0, 0, CRAB_SCR_W, 18, 0x018B);  // Deep lagoon blue
    spr.fillRect(0, 18, CRAB_SCR_W, 8, 0x02B0);  // Shallow turquoise

    // Gentle animated wave foam line (local y = 26..30)
    int foamShift = (int)(sinf(timeSec * 1.6f) * 3.0f);
    spr.fillRect(0, 26 + foamShift, CRAB_SCR_W, 3, 0x9F7F); // Soft seafoam

    // 2. Wet sand & warm golden beach sand (local y = 29..143)
    spr.fillRect(0, 29 + foamShift, CRAB_SCR_W, 12, 0x9B86); // Wet darker sand
    spr.fillRect(0, 41 + foamShift, CRAB_SCR_W, BAND_H - (41 + foamShift), 0xE58B); // Warm golden sand
    spr.fillRect(0, 118, CRAB_SCR_W, 26, 0xC468); // Foreground sand shadow

    // 3. Subtle sand speckles & tiny decorative beach pebbles
    const uint16_t speckCols[] = {0xD4C9, 0xF691, 0xB3A6};
    for (int i = 0; i < 42; i++)
    {
        int px = ((i * 73) + driftOffset) % CRAB_SCR_W;
        if (px < 0) px += CRAB_SCR_W;
        int py = 46 + ((i * 29) % 92);
        spr.fillCircle(px, py, (i % 5 == 0) ? 2 : 1, speckCols[i % 3]);
    }

    // Tiny starfish / seashell detail on the sand
    int starSx = (58 + driftOffset + CRAB_SCR_W) % CRAB_SCR_W;
    int starSy = 124;
    spr.drawLine(starSx - 4, starSy, starSx + 4, starSy, 0xF986);
    spr.drawLine(starSx, starSy - 4, starSx, starSy + 4, 0xF986);
    spr.drawLine(starSx - 3, starSy - 3, starSx + 3, starSy + 3, 0xF986);
    spr.drawLine(starSx - 3, starSy + 3, starSx + 3, starSy - 3, 0xF986);
}

// Draw the gorgeous spiral conch shell centered around (cx, cy)
// dir: 1 = facing right (shell spirals to left), -1 = facing left (shell spirals to right)
static void drawGorgeousShell(TFT_eSprite &spr, int cx, int cy, int dir, float tiltAngle)
{
    // Gorgeous coral, sunset-peach, cream, and magenta-rose palette
    const uint16_t colOuter    = 0xFA68; // Warm coral-salmon
    const uint16_t colMid      = 0xFD4C; // Peachy sunset orange
    const uint16_t colCream    = 0xFF77; // Warm pearl cream
    const uint16_t colTip      = 0xE26B; // Rose-magenta apex
    const uint16_t colRim      = 0x78E2; // Crisp dark outline

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

    // 2. Whorl 3 (smaller rear spiral chamber)
    int w3x = cx + backDir * 21;
    int w3y = cy - 10 + bobY;
    spr.fillCircle(w3x, w3y, 12, colCream);
    spr.drawCircle(w3x, w3y, 12, colRim);
    spr.fillCircle(w3x - backDir * 2, w3y - 3, 4, TFT_WHITE);

    // 3. Whorl 2 (middle chamber)
    int w2x = cx + backDir * 11;
    int w2y = cy - 8 + bobY;
    spr.fillCircle(w2x, w2y, 16, colMid);
    spr.drawCircle(w2x, w2y, 16, colRim);
    spr.drawArc(w2x, w2y, 13, 11, 200, 340, colCream, colMid);
    spr.fillCircle(w2x - backDir * 3, w2y - 5, 4, TFT_WHITE);

    // 4. Main Body Whorl (large front chamber)
    int w1x = cx - backDir * 2;
    int w1y = cy - 5 + bobY;
    spr.fillCircle(w1x, w1y, 21, colOuter);
    spr.drawCircle(w1x, w1y, 21, colRim);
    spr.drawArc(w1x, w1y, 17, 15, 190, 350, colCream, colOuter);
    spr.drawArc(w1x, w1y, 12, 10, 200, 340, colMid, colOuter);
    spr.fillCircle(w1x - backDir * 4, w1y - 8, 5, 0xFFDF);
    spr.fillCircle(w1x - backDir * 8, w1y - 4, 2, TFT_WHITE);

    // 5. Dark hollow shell opening (aperture) facing forward
    int openX = cx + dir * 13;
    int openY = cy + 3 + bobY;
    spr.fillEllipse(openX, openY, 10, 14, 0x3041);
    spr.drawEllipse(openX, openY, 11, 15, colCream);
    spr.drawEllipse(openX, openY, 12, 16, colRim);
}

// Draw cute cartoon eyes on stalks + twitching antennae
static void drawCrabEyesAndAntennae(TFT_eSprite &spr, int baseX, int baseY, int dir,
                                    float lookX, float lookY, bool blinking, float peekRatio)
{
    if (peekRatio <= 0.05f)
        return;

    const uint16_t colBody = 0xFA03; // Bright cute crab orange-red
    const uint16_t colRim  = 0x78E2;

    int stalkH = (int)(13.0f * peekRatio);
    int eyeR = (peekRatio > 0.7f) ? 6 : 5;

    // Two eye stalks side-by-side
    int leftEyeX  = baseX + dir * 4 - 5;
    int rightEyeX = baseX + dir * 4 + 6;
    int eyeY      = baseY - stalkH;

    spr.fillRect(leftEyeX - 1, eyeY, 3, stalkH + 2, colBody);
    spr.fillRect(rightEyeX - 1, eyeY, 3, stalkH + 2, colBody);

    // Twitching antennae
    int antTipX1 = leftEyeX - 7 - (int)(lookX * 1.5f);
    int antTipX2 = rightEyeX + 7 + (int)(lookX * 1.5f);
    int antTipY  = eyeY - 6;
    spr.drawLine(leftEyeX - 2, eyeY + 4, antTipX1, antTipY, colBody);
    spr.drawLine(rightEyeX + 2, eyeY + 4, antTipX2, antTipY, colBody);

    // Eyeballs
    int ex[] = {leftEyeX, rightEyeX};
    for (int i = 0; i < 2; i++)
    {
        if (blinking)
        {
            spr.fillCircle(ex[i], eyeY, eyeR, colBody);
            spr.drawCircle(ex[i], eyeY, eyeR, colRim);
            spr.drawFastHLine(ex[i] - eyeR + 1, eyeY, (eyeR * 2) - 2, colRim);
        }
        else
        {
            spr.fillCircle(ex[i], eyeY, eyeR, TFT_WHITE);
            spr.drawCircle(ex[i], eyeY, eyeR, colRim);

            int px = ex[i] + (int)constrain(lookX, -2.0f, 2.0f);
            int py = eyeY + (int)constrain(lookY, -2.0f, 2.0f);
            spr.fillCircle(px, py, 3, TFT_BLACK);
            spr.fillCircle(px - 1, py - 1, 1, TFT_WHITE);
        }
    }
}

// Render a complete frame of the hermit crab in its current state
static void renderHermitCrabFrame(float timeSec, int driftOffset)
{
    if (!ensureCrabSprite())
        return;

    drawBeachBackground(crabSpr, timeSec, driftOffset);

    int cx = (int)roundf(crabX);
    int groundY = 96; // Sand contact line inside sprite

    // Soft ground shadow beneath the crab
    int shadowW = (crabState == CRAB_WALKING) ? 34 : 28;
    crabSpr.fillEllipse(cx, groundY + 4, shadowW, 5, 0xA365);

    const uint16_t colCrab      = 0xFA03; // Bright cheerful orange-red
    const uint16_t colCrabLight = 0xFC86; // Warm claw/face highlight
    const uint16_t colCrabDark  = 0xB181; // Back leg shading
    const uint16_t colRim       = 0x78E2;

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

        // 3. Cute crab head/face peeking out of the shell aperture
        int headX = cx + crabDir * 14;
        int headY = bodyY + 1;
        crabSpr.fillEllipse(headX, headY, 9, 7, colCrab);
        crabSpr.drawEllipse(headX, headY, 9, 7, colRim);

        // Cute tiny smile & rosy cheeks
        crabSpr.fillCircle(headX + crabDir * 4, headY + 2, 2, 0xFBAE);
        crabSpr.drawPixel(headX + crabDir * 2, headY + 3, colRim);
        crabSpr.drawPixel(headX + crabDir * 3, headY + 4, colRim);
        crabSpr.drawPixel(headX + crabDir * 4, headY + 3, colRim);

        // 4. Cute front pincer claw
        int clawWave = (int)(sinf(walkPhase * 0.7f) * 2.0f);
        int bigClawX = headX + crabDir * 11;
        int bigClawY = headY + 3 + clawWave;
        crabSpr.fillEllipse(bigClawX, bigClawY, 7, 5, colCrabLight);
        crabSpr.drawEllipse(bigClawX, bigClawY, 7, 5, colRim);
        crabSpr.drawLine(bigClawX + crabDir * 2, bigClawY, bigClawX + crabDir * 7, bigClawY - 1, colRim);

        // 5. Bobbing eye stalks & blinking
        bool blink = ((int)(timeSec * 10.0f) % 37 == 0);
        drawCrabEyesAndAntennae(crabSpr, headX, headY - 4, crabDir, (float)crabDir * 1.5f, 0.0f, blink, 1.0f);
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
        return true; // Wake up!
    }
}

// Main screensaver update loop (keeps existing signature for google-ai-main.cpp)
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
        crabDir = 1;
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
        walkPhase += 0.28f;

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
