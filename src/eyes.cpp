#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// Pre-baked 48x48 high-detail iris texture (~4.6 KB RAM) computed once at startup
constexpr int IRIS_TEX_SIZE = 48;
uint16_t irisTex[IRIS_TEX_SIZE * IRIS_TEX_SIZE];
bool irisTexReady = false;

// ============================================================================
// Blinking Eyes Screen Saver (Activates after SCREENSAVER_TIMEROUT_MS of no touch)
// ============================================================================

// Persistent off-screen sprite (~38.9 KB) allocated once to avoid heap fragmentation
TFT_eSprite eyeSpr(&tft);
bool eyeSprReady = false;

// TFT_eSprite eyeSpr(&tft);
TFT_eSprite eyeBgLeft(&tft);
TFT_eSprite eyeBgRight(&tft);
bool eyeAssetsReady = false;

// Fast 16-bit RGB565 linear colour blend (alpha: 0 = bg, 255 = fg)
// Fixed 16-bit RGB565 linear colour blend using signed channel deltas (prevents yellow underflow)
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
    if (alpha == 0)
        return bg;
    if (alpha == 255)
        return fg;

    int32_t bgR = (bg >> 11) & 0x1F;
    int32_t bgG = (bg >> 5) & 0x3F;
    int32_t bgB = bg & 0x1F;

    int32_t fgR = (fg >> 11) & 0x1F;
    int32_t fgG = (fg >> 5) & 0x3F;
    int32_t fgB = fg & 0x1F;

    uint16_t r = (uint16_t)(bgR + (((fgR - bgR) * alpha) >> 8));
    uint16_t g = (uint16_t)(bgG + (((fgG - bgG) * alpha) >> 8));
    uint16_t b = (uint16_t)(bgB + (((fgB - bgB) * alpha) >> 8));

    return (r << 11) | (g << 5) | b;
}

bool ensureEyeSprite()
{
    if (!eyeSprReady)
    {
        eyeSpr.setColorDepth(16);
        if (eyeSpr.createSprite(152, 128) == nullptr)
        {
            Serial.println("Failed to allocate eyeSpr - low heap!");
            return false;
        }
        eyeSprReady = true;
    }
    return true;
}

static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c << 8) | (c >> 8));
}

// Fast bilinear texture lookup for smooth sub-pixel iris rendering
static inline uint16_t sampleIrisBilinear(float u, float v)
{
    float fx = constrain((u * 0.5f + 0.5f) * (IRIS_TEX_SIZE - 1), 0.0f, (float)(IRIS_TEX_SIZE - 2));
    float fy = constrain((v * 0.5f + 0.5f) * (IRIS_TEX_SIZE - 1), 0.0f, (float)(IRIS_TEX_SIZE - 2));
    int ix = (int)fx, iy = (int)fy;
    uint8_t wx = (uint8_t)((fx - ix) * 255.0f);
    uint8_t wy = (uint8_t)((fy - iy) * 255.0f);

    int idx = iy * IRIS_TEX_SIZE + ix;
    uint16_t top = blend565(irisTex[idx], irisTex[idx + 1], wx);
    uint16_t bot = blend565(irisTex[idx + IRIS_TEX_SIZE], irisTex[idx + IRIS_TEX_SIZE + 1], wx);
    return blend565(top, bot, wy);
}

// Pre-baked 48x48 multi-zone anatomical iris texture (~4.6 KB RAM)

void buildIrisTexture()
{
    if (irisTexReady)
        return;
    const float half = (IRIS_TEX_SIZE - 1) * 0.5f;

    for (int y = 0; y < IRIS_TEX_SIZE; y++)
    {
        float dy = ((float)y - half) / half;
        for (int x = 0; x < IRIS_TEX_SIZE; x++)
        {
            float dx = ((float)x - half) / half;
            float r = sqrtf(dx * dx + dy * dy);
            if (r > 1.0f)
            {
                irisTex[y * IRIS_TEX_SIZE + x] = 0x0000;
                continue;
            }

            float angle = atan2f(dy, dx);
            float twist = sinf(r * 6.5f) * 0.12f;
            float a = angle + twist;

            // Fine radial stroma trabeculae + deep anatomical Fuchs' crypts
            float f1 = sinf(a * 36.0f + sinf(a * 7.0f) * 1.5f);
            float f2 = cosf(a * 61.0f - r * 11.0f);
            float f3 = sinf(a * 19.0f + cosf(r * 14.0f) * 1.3f);
            float crypt = sinf(a * 13.0f + 0.9f) * sinf(r * 15.0f - 0.5f);

            float fibre = constrain(0.50f + 0.25f * f1 + 0.15f * f2 + 0.10f * f3 - max(0.0f, crypt) * 0.30f, 0.0f, 1.0f);

            // Rich deep ocean-sapphire to luminous cerulean-teal stroma
            uint16_t col = blend565(0x014A, 0x573F, (uint8_t)(fibre * 255.0f));

            // Jagged anatomical collarette ridge (r ~ 0.44 with star-like peaks)
            float collarRadius = 0.44f + 0.045f * sinf(a * 14.0f) + 0.025f * cosf(a * 23.0f);
            float collarDist = fabsf(r - collarRadius);
            if (collarDist < 0.14f)
            {
                float collarAmt = (1.0f - collarDist / 0.14f) * (0.60f + 0.40f * f1);
                col = blend565(col, 0xCC84, (uint8_t)constrain((int)(collarAmt * 210.0f), 0, 255));
            }

            // Subtle dark stromal furrow rings near outer iris
            if (r > 0.65f && r < 0.82f)
            {
                float furrow = max(0.0f, sinf(r * 38.0f));
                col = blend565(col, 0x00C6, (uint8_t)(furrow * 90.0f));
            }

            // Soft dark outer limbal ring
            if (r > 0.79f)
            {
                float limbal = (r - 0.79f) / 0.21f;
                col = blend565(col, 0x0042, (uint8_t)(limbal * 245.0f));
            }

            irisTex[y * IRIS_TEX_SIZE + x] = col;
        }
    }
    irisTexReady = true;
}

void renderAndPushOneEye(int screenX, int screenY, float lookX, float lookY, float openness, float pupilScale, bool isLeftEye)
{
    if (!ensureEyeSprite())
        return;
    buildIrisTexture();

    const int sprW = 152, sprH = 128;
    const int cx = 76, cy = 64;
    const int halfW = 60, maxUpperH = 29, maxLowerH = 22;

    uint16_t *ptr = (uint16_t *)eyeSpr.getPointer();
    if (!ptr)
        return;

    float lidTrack = constrain(1.0f - (lookY * 0.028f), 0.78f, 1.16f);
    int upperH = (int)(maxUpperH * openness * lidTrack);
    int lowerH = (int)(maxLowerH * openness * (1.0f + (lookY * 0.012f)));
    int lidMeetY = cy + (int)((1.0f - openness) * 7.5f) + (int)(lookY * 0.25f);

    // 1. Soft photographic skin vignette around the eye (eliminates the "floating in black void" look)
    for (int y = 0; y < sprH; y++)
    {
        float ny = (float)(y - cy) * (1.0f / 64.0f);
        float nySq = ny * ny;
        // Fade smoothly to black at top/bottom sprite borders so there is never a hard box edge
        float vMask = max(0.0f, 1.0f - nySq * nySq);
        int rowOffset = y * sprW;

        for (int x = 0; x < sprW; x++)
        {
            float nx = (float)(x - cx) * (1.0f / 76.0f);
            float nxSq = nx * nx;
            float hMask = max(0.0f, 1.0f - nxSq * nxSq);

            // Orbital socket shadow + soft warm light on upper lid & lower cheekbone
            float distFromLid = fabsf((float)(y - lidMeetY)) * (1.0f / 42.0f);
            float browHighlight = max(0.0f, 1.0f - fabsf(ny + 0.58f) * 2.2f);
            float cheekHighlight = max(0.0f, 1.0f - fabsf(ny - 0.52f) * 2.4f);
            float skinLight = constrain((0.22f + browHighlight * 0.42f + cheekHighlight * 0.36f + distFromLid * 0.15f) * vMask * hMask, 0.0f, 1.0f);

            // Subtle micro-grain so skin looks organic rather than plastic
            int grain = ((x * 19 + y * 37) & 7) - 3;
            uint8_t alpha = (uint8_t)constrain((int)(skinLight * 210.0f) + grain, 0, 255);

            // Warm low-key skin tone (0x5227) blended seamlessly into black vignette
            ptr[rowOffset + x] = swap16(blend565(0x0000, 0x6289, alpha));
        }
    }

    // 2. Organic upper eyelid crease & soft lower-lid bag shadow
    for (int dx = -halfW + 3; dx <= halfW - 3; dx++)
    {
        float nx = (float)dx * (1.0f / 60.0f);
        float arch = 1.0f - nx * nx;
        int creaseY = lidMeetY - (int)((upperH + 9) * arch);
        for (int dy = -2; dy <= 2; dy++)
        {
            int py = creaseY + dy;
            if (py >= 0 && py < sprH)
            {
                int idx = py * sprW + (cx + dx);
                uint8_t shadowA = (uint8_t)((165 - abs(dy) * 55) * arch);
                ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x18A2, shadowA));
            }
        }
        int lowerBagY = lidMeetY + (int)((lowerH + 9) * arch);
        if (lowerBagY >= 0 && lowerBagY < sprH)
        {
            int idx = lowerBagY * sprW + (cx + dx);
            ptr[idx] = swap16(blend565(swap16(ptr[idx]), 0x20E3, (uint8_t)(arch * 110.0f)));
        }
    }

    // 3. Spherical eyeball, recessed pupil refraction & lit fleshy lower lid rim
    if (openness > 0.05f)
    {
        float irisX = (float)cx + lookX;
        float irisY = (float)cy + lookY;
        const float irisR = 23.5f, invIrisR = 1.0f / irisR;
        const float pupilNorm = 0.37f * pupilScale;
        float gazeSquashX = 1.0f + fabsf(lookX) * 0.010f;
        float pupilShiftU = -lookX * 0.0055f;
        float pupilShiftV = -lookY * 0.0055f;

        for (int dx = -halfW; dx <= halfW; dx++)
        {
            float nx = (float)dx * (1.0f / 60.0f);
            // Organic asymmetric human eyelid curve (peaks slightly toward inner-centre)
            float peakShift = isLeftEye ? (nx + 0.14f) : (nx - 0.14f);
            float upperArch = max(0.0f, 1.0f - peakShift * peakShift * 1.15f);
            float lowerArch = max(0.0f, 1.0f - nx * nx);

            float fTopY = (float)lidMeetY - (float)upperH * upperArch;
            float fBotY = (float)lidMeetY + (float)lowerH * lowerArch;

            int yStart = max(0, (int)floorf(fTopY) - 1);
            int yEnd = min(sprH - 1, (int)ceilf(fBotY) + 3);

            for (int y = yStart; y <= yEnd; y++)
            {
                int idx = y * sprW + (cx + dx);
                uint16_t bgCol = swap16(ptr[idx]);

                // Fleshy lower eyelid rim (tarsal margin) catching overhead light
                float distBelowBot = (float)y - fBotY;
                if (distBelowBot > 0.0f && distBelowBot <= 3.0f && fabsf(nx) < 0.92f)
                {
                    float rimFade = (1.0f - distBelowBot * 0.33f) * (1.0f - nx * nx * 0.4f);
                    uint16_t rimCol = (distBelowBot < 1.1f) ? 0xCE59 : 0x834B; // Wet highlight edge + warm skin rim
                    ptr[idx] = swap16(blend565(bgCol, rimCol, (uint8_t)(rimFade * 215.0f * openness)));
                    continue;
                }

                float lidCov = min(constrain((float)y - fTopY + 0.5f, 0.0f, 1.0f),
                                   constrain(fBotY - (float)y + 0.5f, 0.0f, 1.0f));
                if (lidCov <= 0.01f)
                    continue;

                uint16_t pixelCol;
                float innerDist = isLeftEye ? (float)(halfW - dx) : (float)(dx + halfW);

                // Soft shaded inner corner (plica & caruncle)
                if (innerDist < 9.5f && abs(y - (int)((fTopY + fBotY) * 0.5f)) <= 3)
                {
                    pixelCol = (innerDist > 6.5f) ? 0xB596 : 0x8C51;
                }
                else
                {
                    float u = ((float)(cx + dx) - irisX) * gazeSquashX * invIrisR;
                    float v = ((float)y - irisY) * invIrisR;
                    float rSq = u * u + v * v;

                    // Deep upper-lid shadow gradient across the spherical sclera
                    float distFromTop = (float)y - fTopY;
                    float lidShadow = constrain(distFromTop * 0.095f, 0.28f, 1.0f);
                    float sphereShade = constrain((1.0f - nx * nx * 0.35f) * lidShadow, 0.26f, 1.0f);
                    uint16_t scleraCol = blend565(0x4228, 0xF7BE, (uint8_t)(sphereShade * 255.0f));

                    if (rSq <= 1.08f)
                    {
                        float r = sqrtf(rSq);
                        float pu = u - pupilShiftU;
                        float pv = v - pupilShiftV;
                        float pr = sqrtf(pu * pu + pv * pv);

                        if (pr <= pupilNorm - 0.03f)
                        {
                            pixelCol = 0x0000;
                        }
                        else if (pr <= pupilNorm + 0.04f)
                        {
                            uint8_t pa = (uint8_t)(((pr - (pupilNorm - 0.03f)) / 0.07f) * 255.0f);
                            pixelCol = blend565(0x0000, 0x9B42, pa);
                        }
                        else
                        {
                            uint16_t baseIris = sampleIrisBilinear(u, v);

                            // Soft directional light caustic on lower-right iris bowl
                            float caustic = constrain((u + v) * 0.25f, 0.0f, 0.35f);
                            if (caustic > 0.01f)
                            {
                                baseIris = blend565(baseIris, 0x9F9F, (uint8_t)(caustic * 255.0f));
                            }

                            // Soft eyelash & upper-lid shadow cast across top third of iris
                            baseIris = blend565(0x0021, baseIris, (uint8_t)(constrain(distFromTop * 0.105f, 0.18f, 1.0f) * 255.0f));

                            pixelCol = (r > 0.92f)
                                           ? blend565(scleraCol, baseIris, (uint8_t)(constrain((1.05f - r) * 7.69f, 0.0f, 1.0f) * 255.0f))
                                           : baseIris;
                        }
                    }
                    else
                    {
                        pixelCol = scleraCol;
                    }
                }

                // Blend softly into dark lashline shadow along eyelid border
                ptr[idx] = swap16((lidCov < 0.99f) ? blend565(0x18A2, pixelCol, (uint8_t)(lidCov * 255.0f)) : pixelCol);
            }
        }

        // 4. Soft-edged studio window catchlight + subtle secondary glint
        if (openness > 0.28f)
        {
            int hx = (int)(irisX - 7.0f - lookX * 0.16f);
            int hy = (int)(irisY - 7.0f - lookY * 0.16f);
            eyeSpr.fillCircle(hx, hy, 3, TFT_WHITE);
            eyeSpr.fillCircle(hx + 3, hy - 2, 2, 0xEF7D);
            eyeSpr.fillCircle(hx - 2, hy + 3, 2, 0xD71C);
            // Eyelash shadow line cutting organically through top of catchlight
            eyeSpr.drawLine(hx - 1, hy - 3, hx + 1, hy + 1, 0x2104);
            eyeSpr.fillCircle((int)(irisX + 7.0f - lookX * 0.10f), (int)(irisY + 6.0f - lookY * 0.10f), 1, 0xB5F6);
        }
    }

    // 5. Organic, slightly irregular upper & lower eyelashes (avoids uniform "comb" look)
    for (int dx = -halfW; dx <= halfW; dx++)
    {
        float nx = (float)dx * (1.0f / 60.0f);
        float peakShift = isLeftEye ? (nx + 0.14f) : (nx - 0.14f);
        float upperArch = max(0.0f, 1.0f - peakShift * peakShift * 1.15f);
        float lowerArch = max(0.0f, 1.0f - nx * nx);

        int topY = (int)roundf((float)lidMeetY - (float)upperH * upperArch);
        int botY = (int)roundf((float)lidMeetY + (float)lowerH * lowerArch);

        if (topY >= 1 && topY < sprH)
        {
            int idx1 = topY * sprW + (cx + dx);
            int idx2 = (topY - 1) * sprW + (cx + dx);
            ptr[idx1] = swap16(blend565(swap16(ptr[idx1]), 0x1082, 220));
            ptr[idx2] = swap16(blend565(swap16(ptr[idx2]), 0x2104, 150));
        }

        // Irregular organic lash clusters (varies spacing, length, and curl slightly per hair)
        int hash = abs((dx * 17 + (isLeftEye ? 11 : 29)) % 13);
        if (openness > 0.18f && (hash == 0 || hash == 4 || hash == 9) && abs(dx) < halfW - 3)
        {
            float side = isLeftEye ? -nx : nx;
            int lenJitter = (hash == 4) ? 2 : ((hash == 9) ? -1 : 0);
            int lashLen = max(4, (int)(7.0f + side * 5.0f) + lenJitter);
            float curlX = side * 6.5f + ((hash == 0) ? 1.2f : -0.8f);

            for (int s = 1; s <= lashLen; s++)
            {
                float t = (float)s / (float)lashLen;
                float fx = (float)(cx + dx) + curlX * t * t;
                int ix = (int)floorf(fx);
                float frac = fx - (float)ix;
                int ly = topY - 1 - s;
                if (ly < 0 || ix < 0 || ix + 1 >= sprW)
                    break;

                uint8_t alpha = (uint8_t)((1.0f - t * 0.75f) * 235.0f);
                int lidx = ly * sprW + ix;
                ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x1082, (uint8_t)(alpha * (1.0f - frac))));
                ptr[lidx + 1] = swap16(blend565(swap16(ptr[lidx + 1]), 0x1082, (uint8_t)(alpha * frac)));
            }
        }

        // Sparse, natural lower lashes
        int lowHash = abs((dx * 23 + (isLeftEye ? 7 : 19)) % 17);
        if (openness > 0.45f && (lowHash == 2 || lowHash == 11) && abs(dx) < halfW - 9)
        {
            float side = isLeftEye ? -nx : nx;
            int curlX = (int)(side * 2.5f);
            for (int s = 1; s <= 3; s++)
            {
                int ly = botY + 2 + s;
                int lx = cx + dx + (curlX * s) / 3;
                if (ly < sprH && lx >= 0 && lx < sprW)
                {
                    int lidx = ly * sprW + lx;
                    ptr[lidx] = swap16(blend565(swap16(ptr[lidx]), 0x2945, (uint8_t)(165 - s * 40)));
                }
            }
        }
    }
    eyeSpr.pushSprite(screenX, screenY);
}
void drawEyePair(float openness, float pupilOffsetX, float pupilOffsetY, float pupilScale, int driftX = 0, int driftY = 0)
{
    int leftX = constrain(8 + driftX, 0, 168);
    int rightX = constrain(160 + driftX, 0, 168);
    int posY = constrain(56 + driftY, 0, 112);

    renderAndPushOneEye(leftX, posY, pupilOffsetX, pupilOffsetY, openness, pupilScale, true);
    renderAndPushOneEye(rightX, posY, pupilOffsetX, pupilOffsetY, openness, pupilScale, false);
}

void updateScreenSaverEyes(bool forceInit = false)
{
    static unsigned long nextActionMs = 0;
    static unsigned long lastMicroMs = 0;
    static int blinkStep = 0;
    static bool doubleBlink = false;
    static float curX = 0.0f, curY = 0.0f;
    static float tgtX = 0.0f, tgtY = 0.0f;
    static float curPupil = 1.0f, tgtPupil = 1.0f;
    static float squintFactor = 1.0f, tgtSquint = 1.0f;
    static int lastDriftX = 0, lastDriftY = 0;

    const float blinkCurve[] = {1.0f, 0.45f, 0.0f, 0.28f, 0.65f, 0.90f};
    unsigned long now = millis();

    // Compute slow Lissajous burn-in drift across the 320x240 display
    float tSec = (float)now * 0.001f;
    int driftX = (int)roundf(sinf(tSec * 0.17f) * 8.0f);  // ±8px horizontal drift
    int driftY = (int)roundf(cosf(tSec * 0.11f) * 48.0f); // ±48px vertical drift

    if (forceInit)
    {
        tft.fillScreen(TFT_BLACK);
        blinkStep = 0;
        doubleBlink = false;
        curX = curY = tgtX = tgtY = 0.0f;
        curPupil = tgtPupil = 1.0f;
        squintFactor = tgtSquint = 1.0f;
        lastDriftX = driftX;
        lastDriftY = driftY;
        drawEyePair(1.0f, 0.0f, 0.0f, 1.0f, driftX, driftY);
        nextActionMs = now + random(1600, 3200);
        lastMicroMs = now;
        return;
    }

    // Clear thin 2px edge strips whenever the sprite shifts position to prevent trails
    if (driftX != lastDriftX || driftY != lastDriftY)
    {
        int oldLeftX = constrain(8 + lastDriftX, 0, 168);
        int oldRightX = constrain(160 + lastDriftX, 0, 168);
        int oldPosY = constrain(56 + lastDriftY, 0, 112);
        int newLeftX = constrain(8 + driftX, 0, 168);
        int newRightX = constrain(160 + driftX, 0, 168);
        int newPosY = constrain(56 + driftY, 0, 112);

        if (newPosY > oldPosY)
        {
            tft.fillRect(0, oldPosY, 320, newPosY - oldPosY, TFT_BLACK);
        }
        else if (newPosY < oldPosY)
        {
            tft.fillRect(0, newPosY + 128, 320, oldPosY - newPosY, TFT_BLACK);
        }
        if (newLeftX > oldLeftX)
        {
            tft.fillRect(oldLeftX, oldPosY, newLeftX - oldLeftX, 128, TFT_BLACK);
            tft.fillRect(oldRightX, oldPosY, newRightX - oldRightX, 128, TFT_BLACK);
        }
        else if (newLeftX < oldLeftX)
        {
            tft.fillRect(newLeftX + 152, oldPosY, oldLeftX - newLeftX, 128, TFT_BLACK);
            tft.fillRect(newRightX + 152, oldPosY, oldRightX - newRightX, 128, TFT_BLACK);
        }

        lastDriftX = driftX;
        lastDriftY = driftY;
    }

    // Smooth saccadic gaze glide + subtle squint & pupil transitions
    bool needsRedraw = false;
    if (fabsf(tgtX - curX) > 0.20f || fabsf(tgtY - curY) > 0.20f)
    {
        curX += (tgtX - curX) * 0.48f;
        curY += (tgtY - curY) * 0.48f;
        if (fabsf(tgtX - curX) <= 0.20f)
            curX = tgtX;
        if (fabsf(tgtY - curY) <= 0.20f)
            curY = tgtY;
        needsRedraw = true;
    }
    if (fabsf(tgtPupil - curPupil) > 0.008f || fabsf(tgtSquint - squintFactor) > 0.01f)
    {
        curPupil += (tgtPupil - curPupil) * 0.22f;
        squintFactor += (tgtSquint - squintFactor) * 0.20f;
        needsRedraw = true;
    }

    // Subtle living ocular micro-drift & pupil breathing every ~90ms
    float microX = 0.0f, microY = 0.0f, breathPupil = 0.0f;
    if (blinkStep == 0 && (now - lastMicroMs >= 90))
    {
        lastMicroMs = now;
        microX = sinf(tSec * 2.1f) * 0.45f;
        microY = cosf(tSec * 1.7f) * 0.30f;
        breathPupil = sinf(tSec * 1.4f) * 0.035f;
        needsRedraw = true;
    }

    if (needsRedraw && blinkStep == 0)
    {
        drawEyePair(squintFactor, curX + microX, curY + microY, curPupil + breathPupil, driftX, driftY);
    }

    if (now >= nextActionMs)
    {
        if (blinkStep == 0)
        {
            int roll = random(10);
            if (roll < 5)
            {
                blinkStep = 1;
                doubleBlink = (random(4) == 0);
                drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil, driftX, driftY);
                nextActionMs = now + 22;
            }
            else if (roll < 8)
            {
                tgtX = (float)random(-17, 18);
                tgtY = (float)random(-6, 6);
                tgtPupil = 0.86f + (float)random(0, 28) * 0.01f;
                tgtSquint = (random(4) == 0) ? 0.84f : 1.0f;
                nextActionMs = now + random(1400, 3200);
            }
            else
            {
                tgtX = 0.0f;
                tgtY = 0.0f;
                tgtPupil = 1.0f;
                tgtSquint = 1.0f;
                nextActionMs = now + random(2000, 4200);
            }
        }
        else
        {
            blinkStep++;
            if (blinkStep >= 6)
            {
                blinkStep = 0;
                drawEyePair(squintFactor, curX, curY, curPupil, driftX, driftY);
                if (doubleBlink)
                {
                    doubleBlink = false;
                    nextActionMs = now + 120;
                }
                else
                {
                    nextActionMs = now + random(2200, 5000);
                }
            }
            else
            {
                drawEyePair(blinkCurve[blinkStep] * squintFactor, curX, curY, curPupil, driftX, driftY);
                nextActionMs = now + (blinkStep == 2 ? 55 : 24);
            }
        }
    }
}
