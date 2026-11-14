
#include <ClevoService.hpp>

OSDefineMetaClassAndStructors(ClevoService, IOService)

#pragma mark ---------- Main Kernel Routines ----------

bool ClevoService::init(OSDictionary *dict)
{
    bool result = IOService::init(dict);

    if (!result)
        return false;

    device          = NULL;
    gWorkLoop       = NULL;
    gNVRAMTimer     = NULL;
    gKBDTimer       = NULL;
    gKBDDimTimer    = NULL;

    gKBDTime        = KBD_DEF_TIME;
    gKBDInUse       = 0;
    gKBLT           = 0;
    gOldKBLT        = 0;
    vendorID        = 0;
    notify          = NULL;
    gKBDService     = NULL;

    gKBDIsOFF       = false;
    gDimTimerActive = true;
    gKBDimmed       = true;
    gKBDJustDim     = true;

    origCallback        = NULL;
    origSpecialCallback = NULL;
    
    gKBDEffectsEnabled = true;
    
    gKBDEffectTimer = NULL;

    gKBDAnimationBaseLevel = 0;
    gKBDAnimationPhase = 0;

    gKBDEffectMode = KBD_EFFECT_NONE;
    gKBDBreatheColorMode = KBD_BREATHE_SINGLE_COLOR;

    gKBDEffectActive = false;

    gKBDColorPhase = 0;
    gKBDBreatheColorIndex = 0;

    gKBDDimmingLevel = KBD_DIM_LEVEL;

    gKBDLevel[0] = KBD_L0_LEVEL;
    gKBDLevel[1] = KBD_L1_LEVEL;
    gKBDLevel[2] = KBD_L2_LEVEL;
    gKBDLevel[3] = KBD_L3_LEVEL;

    gKBDCurrentHWLevel = 0;
    gKBDFadeStartLevel = 0;
    gKBDFadeTargetLevel = 0;
    gKBDFadeStep = 0;

    gKBDFading = false;
    gKBDFadeOut = false;
    
    gKBDFadeRed = 0;
    gKBDFadeGreen = 0;
    gKBDFadeBlue = 0;
    gKBDFadeUsesRGB = false;

    return true;
}


IOService* ClevoService::probe(IOService *provider, SInt32 *score)
{
    IOService *result = IOService::probe(provider, score);

    // Check our SSDT methods.
    device = OSDynamicCast(IOACPIPlatformDevice, provider);

    if (device == NULL)
        return NULL;

    if (kIOReturnSuccess != device->validateObject(ACPI_METD_NAME))
    {
        IOLog("%s: SSDT method CLVE not found\n", device->getName());
        return NULL;
    }

    return result;
}


bool ClevoService::start(IOService *provider)
{
    DEBUG_LOG("%s: Starting...\n", getName());

    if (device == NULL || !IOService::start(provider))
        return false;

    setName("ClevoService");
    setVendorID(ClevoVendorSTR);

    gWorkLoop = getWorkLoop();

    if (gWorkLoop == NULL)
        return false;

    gWorkLoop->retain();

    // NVRAM timer.
    gNVRAMTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(
            IOTimerEventSource::Action,
            this,
            &ClevoService::saveNVRAM
        )
    );

    if ((gNVRAMTimer == NULL) ||
        (kIOReturnSuccess != gWorkLoop->addEventSource(gNVRAMTimer)))
    {
        DEBUG_LOG("%s: gNVRAMTimer failed...\n", getName());
        return false;
    }

    // Keyboard inactivity timer.
    gKBDTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(
            IOTimerEventSource::Action,
            this,
            &ClevoService::kbdTimerFired
        )
    );

    if ((gKBDTimer == NULL) ||
        (kIOReturnSuccess != gWorkLoop->addEventSource(gKBDTimer)))
    {
        DEBUG_LOG("%s: gKBDTimer failed...\n", getName());
        return false;
    }

    // Keyboard fade animation timer.
    gKBDDimTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(
            IOTimerEventSource::Action,
            this,
            &ClevoService::kbdDimTimerFired
        )
    );

    if ((gKBDDimTimer == NULL) ||
        (kIOReturnSuccess != gWorkLoop->addEventSource(gKBDDimTimer)))
    {
        DEBUG_LOG("%s: gKBDDimTimer failed...\n", getName());
        return false;
    }
    
    // Keyboard breathing animation timer.
    gKBDEffectTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(
            IOTimerEventSource::Action,
            this,
            &ClevoService::kbdEffectTimerFired
        )
    );

    if ((gKBDEffectTimer == NULL) ||
        (kIOReturnSuccess != gWorkLoop->addEventSource(gKBDEffectTimer)))
    {
        DEBUG_LOG("%s: gKBDEffectTimer failed...\n", getName());

        OSSafeReleaseNULL(gKBDEffectTimer);
    }

    // KBLT bytes: 0x0ABC
    // A: 0-8 keyboard color
    // B: 0/1 backlight OFF/ON
    // C: 0-3 backlight level
    gKBLT = 0x12;

    // Read KBLT variable from NVRAM.
    if (IORegistryEntry *nvram =
        OSDynamicCast(IORegistryEntry, fromPath("/options", gIODTPlane)))
    {
        bool genericNVRAM =
            (0 == strncmp(nvram->getName(), "AppleNVRAM", sizeof("AppleNVRAM")));

        OSObject *obj = NULL;

        if (genericNVRAM)
            obj = nvram->IORegistryEntry::getProperty(KBD_VAR_NAME);
        else
            obj = nvram->getProperty(KBD_VAR_NAME);

        if (obj != NULL)
        {
            if (OSData *number = OSDynamicCast(OSData, obj))
            {
                int length = number->getLength();

                if (length > 0 && length <= sizeof(gKBLT))
                    bcopy(number->getBytesNoCopy(), &gKBLT, length);
            }
        }

        OSSafeReleaseNULL(nvram);
    }

    // Save original KBLT value.
    gOldKBLT = gKBLT;

    // Load user's configuration.
    loadConfiguration();

    // Initialize current hardware brightness.
    gKBDCurrentHWLevel = gKBDLevel[gKBLT & 3];

    gKBDFading = false;
    gKBDFadeOut = false;

    // Set up keyboard backlight.
    kbdSetUpBacklight();

    gKBDService = this;

    loggedKeyboards = new OSArray();

    if (loggedKeyboards == NULL)
        return false;

    loggedKeyboards->initWithCapacity(1);

    DEBUG_LOG("%s: gIOTerminatedNotification...\n", getName());

    notifyTerm = addMatchingNotification(
        gIOTerminatedNotification,
        serviceMatching("IOHIKeyboard"),
        (IOServiceMatchingNotificationHandler)&ClevoService::kbdTerminatedNotificationHandler,
        this,
        0
    );

    DEBUG_LOG("%s: gIOPublishNotification...\n", getName());

    notify = addMatchingNotification(
        gIOPublishNotification,
        serviceMatching("IOHIKeyboard"),
        (IOServiceMatchingNotificationHandler)&ClevoService::kbdNotificationHandler,
        this,
        0
    );

    setProperty(KBD_VAR_NAME, gKBLT, 8 * sizeof(gKBLT));
    setProperty("KbdAutoDimActive", gKBDJustDim);
    setProperty("KbdAutoDimTimerActive", gDimTimerActive);
    setProperty("KbdAutoDimTime", gKBDTime, 8 * sizeof(gKBDTime));
    setProperty("KbdDimmingLevel", gKBDDimmingLevel, 8 * sizeof(gKBDDimmingLevel));
    setProperty("KbdBreathingActive", gKBDEffectMode == KBD_EFFECT_BREATHE);
    setProperty("KbdEffectMode", (UInt32)gKBDEffectMode, 8 * sizeof(UInt32));
    setProperty("KbdBreatheColorMode", (UInt32)gKBDBreatheColorMode, 8 * sizeof(UInt32));

    extern kmod_info_t kmod_info;

    char buf[128];

#ifdef DEBUG
    snprintf(buf, sizeof(buf), "FJHK Debug %s", kmod_info.version);
#else
    snprintf(buf, sizeof(buf), "FJHK Release %s", kmod_info.version);
#endif

    setProperty("Version", buf);

    OSArray *levelsArray = new OSArray();

    if (levelsArray != NULL)
    {
        levelsArray->initWithCapacity(1);

        for (UInt8 i = 0; i < 4; i++)
        {
            OSNumber *myNum = OSNumber::withNumber(gKBDLevel[i], 8);

            if (myNum)
            {
                levelsArray->setObject(i, myNum);
                myNum->release();
            }
        }

        setProperty("KbdLevels", levelsArray);
        levelsArray->release();
    }

    registerService(0);

    IOLog("%s: AutoDim %s\n",
          getName(),
          (gDimTimerActive ? "activated" : "not activated"));

    IOLog("%s: AutoDim %s\n",
          getName(),
          (gKBDJustDim ? "dimming activated" : "OFF activated"));

    IOLog("%s: Backlighting activated: KBLT=0x%x\n",
          getName(),
          gKBLT);

    return true;
}


void ClevoService::stop(IOService *provider)
{
    DEBUG_LOG("%s: Stopping...\n", getName());

    saveNVRAM();

    if (gWorkLoop != NULL)
    {
        // Stop all event sources before removing them.
        gWorkLoop->disableAllEventSources();

        if (gNVRAMTimer != NULL)
        {
            gNVRAMTimer->cancelTimeout();
            gWorkLoop->removeEventSource(gNVRAMTimer);
            gNVRAMTimer->release();
            gNVRAMTimer = NULL;

            DEBUG_LOG("%s: gNVRAMTimer Removed\n", getName());
        }

        if (gKBDTimer != NULL)
        {
            gKBDTimer->cancelTimeout();
            gWorkLoop->removeEventSource(gKBDTimer);
            gKBDTimer->release();
            gKBDTimer = NULL;

            DEBUG_LOG("%s: gKBDTimer Removed\n", getName());
        }

        if (gKBDDimTimer != NULL)
        {
            gKBDDimTimer->cancelTimeout();
            gWorkLoop->removeEventSource(gKBDDimTimer);
            gKBDDimTimer->release();
            gKBDDimTimer = NULL;

            DEBUG_LOG("%s: gKBDDimTimer Removed\n", getName());
        }
        
        if (gKBDEffectTimer != NULL)
        {
            kbdStopEffect(false);

            gKBDEffectTimer->cancelTimeout();
            gWorkLoop->removeEventSource(gKBDEffectTimer);

            gKBDEffectTimer->release();
            gKBDEffectTimer = NULL;

            DEBUG_LOG("%s: gKBDEffectTimer Removed\n", getName());
        }

        gWorkLoop->release();
        gWorkLoop = NULL;

        DEBUG_LOG("%s: WorkLoop Removed\n", getName());
    }

    if (notifyTerm)
        notifyTerm->remove();

    notifyTerm = NULL;

    DEBUG_LOG("%s: TerminatedNotification Removed\n", getName());

    if (notify)
        notify->remove();

    notify = NULL;

    DEBUG_LOG("%s: PublishNotification Removed\n", getName());

    gKBDService = NULL;

    kbdClearKeyboards();

    if (loggedKeyboards != NULL)
    {
        loggedKeyboards->release();
        loggedKeyboards = NULL;
    }

    DEBUG_LOG("%s: Keyboards Removed...\n", getName());

    IOLog("%s: Stop Complete\n", getName());

    IOService::stop(provider);
}


// Receive Notify message from ACPI.
// Wait before writing NVRAM so the user may change other KB settings.
IOReturn ClevoService::message(
    UInt32 type,
    IOService *provider,
    void *argument
)
{
    UInt8 level = gKBLT & 3;
    UInt32 *myMsg;
    UInt32 color = gKBLT & 0xF00;

    if (type == kIOACPIMessageDeviceNotification)
    {
        myMsg = (UInt32 *)argument;

        if (myMsg == NULL)
            return IOService::message(type, provider, argument);

        switch (*myMsg)
        {
            case msgKbdChangeColor:
                if (!gKBDIsOFF && !gKBDEffectsEnabled)
                {
                    gKBLT &= 0xFF;
                    if (color < 0x800)
                        gKBLT |= (color + 0x100);
                    kbdSetColor();
                }
                break;

            case msgKbdLightDOWN:
                if (!gKBDIsOFF)
                {
                    if (level)
                        kbdLightLevel(--level);

                    sendUserLandMessage(evtKeyboardBacklight, level + 1, 4);
                }
                break;

            case msgKbdLightUP:
                if (!gKBDIsOFF)
                {
                    if (level < 3)
                        kbdLightLevel(++level);

                    sendUserLandMessage(evtKeyboardBacklight, level + 1, 4);
                }
                break;

            case msgKbdToggleONOFF:
                if (!gKBDIsOFF)
                {
                    gKBLT &= 0xF0F;
                    gKBDIsOFF = true;
                }
                else
                {
                    gKBLT |= KBD_ON_MASK;
                    gKBDIsOFF = false;
                }

                sendUserLandMessage(
                    evtKeyboardBacklight,
                    !gKBDIsOFF ? level + 1 : 0,
                    4
                );

                kbdSetUpBacklight();
                break;

            case msgKbdWakeUp:
                kbdSetUpBacklight();
                break;

            case msgAirplane:
                sendUserLandMessage(evtAirplaneMode, 0, 0);
                break;

#if DEBUG
            default:
                sendUserLandMessage(3, *myMsg & 0xFFFF, 0);
#endif
        }

        if (gNVRAMTimer != NULL)
        {
            gNVRAMTimer->cancelTimeout();
            gNVRAMTimer->setTimeoutMS(NVRAM_WAIT_TIME);
        }
    }

    return IOService::message(type, provider, argument);
}


#pragma mark ---------- Service Routines ----------

void ClevoService::ACPI_Send(
    uint32_t arg0,
    uint32_t arg1,
    uint32_t arg2
)
{
    OSObject *params[3];

    params[0] = OSNumber::withNumber(arg0, 8 * sizeof(uint32_t));
    params[1] = OSNumber::withNumber(arg1, 8 * sizeof(uint32_t));
    params[2] = OSNumber::withNumber(arg2, 8 * sizeof(uint32_t));

    if (device != NULL)
        device->evaluateObject("CLVE", NULL, params, 3);
    else
        DEBUG_LOG("%s: device NULL!\n", getName());

    OSSafeReleaseNULL(params[0]);
    OSSafeReleaseNULL(params[1]);
    OSSafeReleaseNULL(params[2]);
}


void ClevoService::saveNVRAM(void)
{
    gKBLT &= 0xF13;

    if (gOldKBLT != gKBLT)
    {
        if (IORegistryEntry *nvram =
            OSDynamicCast(IORegistryEntry, fromPath("/options", gIODTPlane)))
        {
            bool genericNVRAM =
                (0 == strncmp(nvram->getName(), "AppleNVRAM", sizeof("AppleNVRAM")));

            if (genericNVRAM)
            {
                nvram->IORegistryEntry::setProperty(
                    KBD_VAR_NAME,
                    &gKBLT,
                    sizeof(gKBLT)
                );

                DEBUG_LOG("%s: fallback to generic NVRAM methods\n", getName());
            }
            else
            {
                nvram->setProperty(KBD_VAR_NAME, &gKBLT, sizeof(gKBLT));
            }

            OSSafeReleaseNULL(nvram);

            gOldKBLT = gKBLT;

            this->setProperty(KBD_VAR_NAME, gKBLT, 8 * sizeof(gKBLT));

            IOLog("%s: NVRAM saved KBLT=0x%x\n", getName(), gKBLT);
        }
    }
}


bool ClevoService::setVendorID(const char *vendorCode)
{
    if (KERN_SUCCESS != kev_vendor_code_find(vendorCode, &vendorID))
    {
        DEBUG_LOG("%s: setVendorID error\n", getName());
        return false;
    }

    return true;
}


bool ClevoService::sendUserLandMessage(
    int type,
    int param1,
    int param2
)
{
    struct kev_msg kEventMsg = {0};

    bzero(&kEventMsg, sizeof(struct kev_msg));

    kEventMsg.vendor_code = vendorID;

    if (vendorID == 0)
        DEBUG_LOG("%s: vendorID not set\n", getName());

    kEventMsg.kev_class = KEV_ANY_CLASS;
    kEventMsg.kev_subclass = KEV_ANY_SUBCLASS;
    kEventMsg.event_code = ClevoEventCode;

    kEventMsg.dv[0].data_length = sizeof(int);
    kEventMsg.dv[0].data_ptr = &type;

    kEventMsg.dv[1].data_length = sizeof(int);
    kEventMsg.dv[1].data_ptr = &param1;

    kEventMsg.dv[2].data_length = sizeof(int);
    kEventMsg.dv[2].data_ptr = &param2;

    if (KERN_SUCCESS != kev_msg_post(&kEventMsg))
    {
        DEBUG_LOG("%s: sendUserLandMessage error\n", getName());
        return false;
    }

    return true;
}

static const UInt8 gKBDPalette[7][3] =
{
    {   0,   0, 255 }, // Blue
    { 255,   0,   0 }, // Red
    { 255, 255, 255 }, // White
    { 255,   0, 255 }, // Pink
    {   0, 255, 255 }, // Light Blue
    {   0, 255,   0 }, // Green
    { 255, 255,   0 }  // Yellow
};

#pragma mark ---------- Keyboard Fade Routines ----------

// 40 steps x 50 ms = 2000 ms.
#define KBD_FADE_STEPS       40
#define KBD_FADE_INTERVAL    50


void ClevoService::kbdSetHardwareLevel(UInt8 level)
{
    ACPI_Send(0, SET_KB_LED, 0xF4000000 | level);

    gKBDCurrentHWLevel = level;
}


void ClevoService::kbdStartFade(UInt8 targetLevel, bool fadeOut)
{
    if (gKBDDimTimer != NULL)
        gKBDDimTimer->cancelTimeout();

    gKBDFadeStartLevel = gKBDCurrentHWLevel;
    gKBDFadeTargetLevel = targetLevel;
    gKBDFadeStep = 0;

    gKBDFadeOut = fadeOut;
    gKBDFading = true;

    // Capture the selected static color.
    gKBDFadeUsesRGB = true;

    switch (gKBLT & 0xF00)
    {
        case 0x000: // Blue
            gKBDFadeRed = 0;
            gKBDFadeGreen = 0;
            gKBDFadeBlue = 255;
            break;

        case 0x100: // Red
            gKBDFadeRed = 255;
            gKBDFadeGreen = 0;
            gKBDFadeBlue = 0;
            break;

        case 0x200: // White
            gKBDFadeRed = 255;
            gKBDFadeGreen = 255;
            gKBDFadeBlue = 255;
            break;

        case 0x300: // Pink
            gKBDFadeRed = 255;
            gKBDFadeGreen = 0;
            gKBDFadeBlue = 255;
            break;

        case 0x400: // Light Blue
            gKBDFadeRed = 0;
            gKBDFadeGreen = 255;
            gKBDFadeBlue = 255;
            break;

        case 0x500: // Green
            gKBDFadeRed = 0;
            gKBDFadeGreen = 255;
            gKBDFadeBlue = 0;
            break;

        case 0x600: // Yellow
            gKBDFadeRed = 255;
            gKBDFadeGreen = 255;
            gKBDFadeBlue = 0;
            break;

        case 0x700: // RGB sequence
        case 0x800: // Random
        default:
            gKBDFadeUsesRGB = false;
            break;
    }

    // Pause any active effect.
    kbdStopEffect(false);

    if (!fadeOut)
    {
        gKBDimmed = false;

        if (gKBDFadeUsesRGB)
        {
            ACPI_Send(0, SET_KB_LED, 0xE007F001);
            kbdSetColorRGB(0, 0, 0);
        }
        else
        {
            kbdSetColor();

            ACPI_Send(0, SET_KB_LED, 0xE007F001);

            kbdSetHardwareLevel(gKBDFadeStartLevel);
        }
    }

    if (!gKBDFadeUsesRGB &&
        gKBDFadeStartLevel == gKBDFadeTargetLevel)
    {
        gKBDFading = false;
        gKBDimmed = fadeOut;
        return;
    }

    if (gKBDDimTimer == NULL)
    {
        if (gKBDFadeUsesRGB)
        {
            kbdSetColorRGB(
                fadeOut ? 0 : gKBDFadeRed,
                fadeOut ? 0 : gKBDFadeGreen,
                fadeOut ? 0 : gKBDFadeBlue
            );
        }
        else
        {
            kbdSetHardwareLevel(targetLevel);
        }

        gKBDFading = false;
        gKBDimmed = fadeOut;
        return;
    }

    gKBDDimTimer->setTimeoutMS(KBD_FADE_INTERVAL);

    DEBUG_LOG(
        "%s: Fade %s started: %u -> %u\n",
        getName(),
        fadeOut ? "OUT" : "IN",
        gKBDFadeStartLevel,
        gKBDFadeTargetLevel
    );
}

void ClevoService::kbdRestoreBacklight()
{
    if ((gKBDimmed || (gKBDFading && gKBDFadeOut)) &&
        (gKBLT & KBD_ON_MASK))
    {
        UInt8 restoreLevel = gKBDLevel[gKBLT & 3];

        kbdStartFade(restoreLevel, false);
    }
}

void ClevoService::kbdDimTimerFired()
{
    if (!gKBDFading)
        return;

    if (gKBDFadeStep < KBD_FADE_STEPS)
        gKBDFadeStep++;

    double t =
        (double)gKBDFadeStep / (double)KBD_FADE_STEPS;

    // Smoothstep: 3t² - 2t³.
    double eased = t * t * (3.0 - 2.0 * t);

    if (gKBDFadeUsesRGB)
    {
        // Fade OUT: 255 -> 0.
        // Fade IN: 0 -> 255.
        double intensity = gKBDFadeOut
            ? (1.0 - eased)
            : eased;

        UInt8 red =
            (UInt8)(gKBDFadeRed * intensity + 0.5);

        UInt8 green =
            (UInt8)(gKBDFadeGreen * intensity + 0.5);

        UInt8 blue =
            (UInt8)(gKBDFadeBlue * intensity + 0.5);

        kbdSetColorRGB(red, green, blue);
    }
    else
    {
        // Original hardware brightness fade.
        SInt32 start = (SInt32)gKBDFadeStartLevel;
        SInt32 target = (SInt32)gKBDFadeTargetLevel;
        SInt32 difference = target - start;

        SInt32 brightness =
            start + (SInt32)(
                difference * eased +
                (difference >= 0 ? 0.5 : -0.5)
            );

        if (brightness < 0)
            brightness = 0;

        if (brightness > 255)
            brightness = 255;

        kbdSetHardwareLevel((UInt8)brightness);
    }

    if (gKBDFadeStep >= KBD_FADE_STEPS)
    {
        if (gKBDFadeUsesRGB)
        {
            // Guarantee exact final RGB intensity.
            if (gKBDFadeOut)
                kbdSetColorRGB(0, 0, 0);
            else
                kbdSetColorRGB(
                    gKBDFadeRed,
                    gKBDFadeGreen,
                    gKBDFadeBlue
                );
        }
        else
        {
            kbdSetHardwareLevel(gKBDFadeTargetLevel);
        }

        gKBDFading = false;
        gKBDimmed = gKBDFadeOut;

        // Resume effects only when they are enabled.
        if (!gKBDimmed &&
            gKBDEffectsEnabled &&
            gKBDEffectMode != KBD_EFFECT_NONE)
        {
            kbdStartEffect();
        }

        DEBUG_LOG(
            "%s: Fade %s completed: %u\n",
            getName(),
            gKBDFadeOut ? "OUT" : "IN",
            gKBDCurrentHWLevel
        );

        return;
    }

    gKBDDimTimer->setTimeoutMS(KBD_FADE_INTERVAL);
}

#pragma mark ---------- Keyboard Effects ----------

void ClevoService::kbdStartEffect()
{
    if (!gKBDEffectsEnabled ||
        gKBDEffectMode == KBD_EFFECT_NONE ||
        gKBDEffectTimer == NULL ||
        gKBDIsOFF ||
        !(gKBLT & KBD_ON_MASK) ||
        gKBDimmed ||
        gKBDFading)
    {
        return;
    }

    gKBDEffectTimer->cancelTimeout();

    gKBDAnimationBaseLevel = gKBDLevel[gKBLT & 3];
    gKBDAnimationPhase = 0;
    gKBDColorPhase = 0;

    gKBDBreatheColorIndex = (gKBLT & 0xF00) >> 8;

    if (gKBDBreatheColorIndex > 6)
        gKBDBreatheColorIndex = 0;

    gKBDEffectActive = true;

    if (gKBDEffectMode == KBD_EFFECT_BREATHE)
    {
        // Apply the selected KbdLevels brightness.
        kbdSetHardwareLevel(gKBDAnimationBaseLevel);

        // Begin at zero RGB intensity.
        kbdSetColorRGB(0, 0, 0);
    }

    else if (gKBDEffectMode == KBD_EFFECT_COLOR_CYCLE)
    {
        // Color cycle must not alter the selected brightness.
        kbdSetColorRGB(
            gKBDPalette[gKBDBreatheColorIndex][0],
            gKBDPalette[gKBDBreatheColorIndex][1],
            gKBDPalette[gKBDBreatheColorIndex][2]
        );
    }

    gKBDEffectTimer->setTimeoutMS(KBD_BREATHE_INTERVAL);

    DEBUG_LOG(
        "%s: Effect started. Mode=%u, Brightness=%u\n",
        getName(),
        gKBDEffectMode,
        gKBDAnimationBaseLevel
    );
}


void ClevoService::kbdStopEffect(bool restore)
{
    if (gKBDEffectTimer != NULL)
        gKBDEffectTimer->cancelTimeout();

    bool wasBreathe = (gKBDEffectMode == KBD_EFFECT_BREATHE);

    gKBDEffectActive = false;
    gKBDAnimationPhase = 0;
    gKBDColorPhase = 0;

    if (!gKBDIsOFF && (gKBLT & KBD_ON_MASK))
    {
        if (restore && wasBreathe && gKBDEffectsEnabled)
        {
            // Restore the breathing effect's selected base color.
            kbdSetColorRGB(
                gKBDPalette[gKBDBreatheColorIndex][0],
                gKBDPalette[gKBDBreatheColorIndex][1],
                gKBDPalette[gKBDBreatheColorIndex][2]
            );
        }
        else if (restore &&
                 !gKBDimmed &&
                 !gKBDFading)
        {
            kbdSetHardwareLevel(gKBDLevel[gKBLT & 3]);
        }
    }
}

void ClevoService::kbdEffectTimerFired()
{
    if (!gKBDEffectActive ||
        gKBDEffectMode == KBD_EFFECT_NONE ||
        gKBDIsOFF ||
        !(gKBLT & KBD_ON_MASK) ||
        gKBDimmed ||
        gKBDFading)
    {
        kbdStopEffect(false);
        return;
    }

    // -------------------------------------------------
    // MODE 1: BREATHING
    // -------------------------------------------------

    if (gKBDEffectMode == KBD_EFFECT_BREATHE)
    {
        const UInt16 FADE_IN_STEPS = 70;
        const UInt16 FADE_OUT_STEPS = 70;
        const UInt16 COLOR_BLEND_STEPS = 3;

        const UInt16 TOTAL_STEPS =
            FADE_IN_STEPS + FADE_OUT_STEPS;

        // Advance animation phase.
        gKBDAnimationPhase++;

        if (gKBDAnimationPhase >= TOTAL_STEPS)
            gKBDAnimationPhase = 0;

        UInt16 phase = gKBDAnimationPhase;

        // ---------------------------------------------
        // CALCULATE RGB INTENSITY
        // ---------------------------------------------

        UInt32 position;

        if (phase <= FADE_IN_STEPS)
        {
            // Fade in: 0 -> 255.
            position = phase;
        }
        else
        {
            // Fade out: 255 -> 0.
            position = TOTAL_STEPS - phase;
        }

        // Normalized smoothstep curve.
        UInt64 smoothNumerator =
            (UInt64)position *
            position *
            (3 * FADE_IN_STEPS - 2 * position);

        UInt64 smoothDenominator =
            (UInt64)FADE_IN_STEPS *
            FADE_IN_STEPS *
            FADE_IN_STEPS;

        UInt8 intensity = (UInt8)(
            (255 * smoothNumerator +
             smoothDenominator / 2) /
            smoothDenominator
        );

        // ---------------------------------------------
        // DETERMINE BASE RGB COLOR
        // ---------------------------------------------

        UInt8 red =
            gKBDPalette[gKBDBreatheColorIndex][0];

        UInt8 green =
            gKBDPalette[gKBDBreatheColorIndex][1];

        UInt8 blue =
            gKBDPalette[gKBDBreatheColorIndex][2];

        // ---------------------------------------------
        // MULTI COLOR: BLEND DURING FINAL FADE-OUT
        // ---------------------------------------------

        if (gKBDBreatheColorMode == KBD_BREATHE_MULTI_COLOR)
        {
            UInt8 colorIndex = gKBDBreatheColorIndex;
            UInt8 nextColorIndex = (colorIndex + 1) % 7;

            if (phase >= TOTAL_STEPS - COLOR_BLEND_STEPS)
            {
                UInt16 colorProgress =
                    phase - (TOTAL_STEPS - COLOR_BLEND_STEPS) + 1;

                red = (UInt8)(
                    (SInt32)gKBDPalette[colorIndex][0] +
                    ((SInt32)gKBDPalette[nextColorIndex][0] -
                     gKBDPalette[colorIndex][0]) *
                    colorProgress / COLOR_BLEND_STEPS
                );

                green = (UInt8)(
                    (SInt32)gKBDPalette[colorIndex][1] +
                    ((SInt32)gKBDPalette[nextColorIndex][1] -
                     gKBDPalette[colorIndex][1]) *
                    colorProgress / COLOR_BLEND_STEPS
                );

                blue = (UInt8)(
                    (SInt32)gKBDPalette[colorIndex][2] +
                    ((SInt32)gKBDPalette[nextColorIndex][2] -
                     gKBDPalette[colorIndex][2]) *
                    colorProgress / COLOR_BLEND_STEPS
                );

                // Advance after completing the transition.
                if (phase == TOTAL_STEPS - 1)
                    gKBDBreatheColorIndex = nextColorIndex;
            }
        }

        // ---------------------------------------------
        // APPLY RGB INTENSITY
        // ---------------------------------------------

        UInt8 outputRed = (UInt8)(
            ((UInt16)red * intensity + 127) / 255
        );

        UInt8 outputGreen = (UInt8)(
            ((UInt16)green * intensity + 127) / 255
        );

        UInt8 outputBlue = (UInt8)(
            ((UInt16)blue * intensity + 127) / 255
        );

        kbdSetColorRGB(
            outputRed,
            outputGreen,
            outputBlue
        );
    }

    // -------------------------------------------------
    // MODE 2: COLOR CYCLE
    // -------------------------------------------------

    else if (gKBDEffectMode == KBD_EFFECT_COLOR_CYCLE)
    {
        const UInt16 COLOR_TRANSITION_STEPS = 35;
        const UInt16 COLOR_CYCLE_STEPS =
            7 * COLOR_TRANSITION_STEPS;

        gKBDColorPhase++;

        if (gKBDColorPhase >= COLOR_CYCLE_STEPS)
            gKBDColorPhase = 0;

        UInt8 colorIndex =
            gKBDColorPhase / COLOR_TRANSITION_STEPS;

        UInt8 nextColorIndex =
            (colorIndex + 1) % 7;

        UInt16 colorProgress =
            gKBDColorPhase % COLOR_TRANSITION_STEPS;

        SInt32 red =
            (SInt32)gKBDPalette[colorIndex][0] +
            ((SInt32)gKBDPalette[nextColorIndex][0] -
             gKBDPalette[colorIndex][0]) *
            colorProgress / COLOR_TRANSITION_STEPS;

        SInt32 green =
            (SInt32)gKBDPalette[colorIndex][1] +
            ((SInt32)gKBDPalette[nextColorIndex][1] -
             gKBDPalette[colorIndex][1]) *
            colorProgress / COLOR_TRANSITION_STEPS;

        SInt32 blue =
            (SInt32)gKBDPalette[colorIndex][2] +
            ((SInt32)gKBDPalette[nextColorIndex][2] -
             gKBDPalette[colorIndex][2]) *
            colorProgress / COLOR_TRANSITION_STEPS;

        kbdSetColorRGB(
            (UInt8)red,
            (UInt8)green,
            (UInt8)blue
        );

        // No brightness command in Color Cycle.
    }

    gKBDEffectTimer->setTimeoutMS(KBD_BREATHE_INTERVAL);
}


#pragma mark ---------- Keyboard Backlight Routines ----------

void ClevoService::kbdSetUpBacklight()
{
    if (gKBLT & KBD_ON_MASK)
    {
        // Keyboard is being initialized in the ON state.
        gKBDimmed = false;

        kbdSetColor();
        kbdONOFF();

        // Apply the saved brightness and start the selected effect.
        kbdLightLevel(gKBLT & 3);
    }
    else
    {
        kbdONOFF();
    }
}

void ClevoService::kbdONOFF()
{
    if (gKBLT & KBD_ON_MASK)
    {
        ACPI_Send(0, SET_KB_LED, 0xE007F001);

        if (gKBDTimer != NULL)
        {
            if (gDimTimerActive)
                gKBDTimer->setTimeoutMS(gKBDTime);
            else
                gKBDTimer->cancelTimeout();
        }
    }
    else
    {
        kbdStopEffect(false);
        
        if (gKBDTimer != NULL)
            gKBDTimer->cancelTimeout();

        ACPI_Send(0, SET_KB_LED, 0xE0003001);
    }
}

void ClevoService::kbdSetColorRGB(
    UInt8 red,
    UInt8 green,
    UInt8 blue
)
{
    // Match the RGB byte ordering used by the existing
    // kbdSetColor() implementation.
    UInt32 color =
        ((UInt32)blue << 16) |
        ((UInt32)red << 8) |
        (UInt32)green;

    // Preserve the existing ACPI color command sequence.
    ACPI_Send(0, SET_KB_LED, 0xF0000000 | color);
    ACPI_Send(0, SET_KB_LED, 0xF1000000 | color);
    ACPI_Send(0, SET_KB_LED, 0xF2000000 | color);
}

void ClevoService::kbdSetColor(void)
{
    UInt32 newColor = 0;
    UInt32 color = gKBLT & 0xF00;

    switch (color)
    {
        case 0x0:
            newColor = 0x00FF0000;
            break; // Blue

        case 0x100:
            newColor = 0x0000FF00;
            break; // Red

        case 0x200:
            newColor = 0x00FFFFFF;
            break; // White

        case 0x300:
            newColor = 0x00FFFF00;
            break; // Pink

        case 0x400:
            newColor = 0x00FF00FF;
            break; // Light Blue

        case 0x500:
            newColor = 0x000000FF;
            break; // Green

        case 0x600:
            newColor = 0x0000FFFF;
            break; // Yellow

        case 0x700:
            // RGB
            ACPI_Send(0, SET_KB_LED, 0xF000FF00);
            ACPI_Send(0, SET_KB_LED, 0xF10000FF);
            ACPI_Send(0, SET_KB_LED, 0xF2FF0000);
            break;

        case 0x800:
            // Random.
            ACPI_Send(0, SET_KB_LED, 0x70000000);
            break;
    }

    if (newColor)
    {
        ACPI_Send(0, SET_KB_LED, 0xF0000000 | newColor);
        ACPI_Send(0, SET_KB_LED, 0xF1000000 | newColor);
        ACPI_Send(0, SET_KB_LED, 0xF2000000 | newColor);
    }
}


void ClevoService::kbdLightLevel(UInt8 newLevel)
{
    newLevel &= 3;

    gKBLT &= 0xFF0;
    gKBLT |= newLevel;

    // Run breathing only when effects are enabled.
    if (gKBDEffectsEnabled &&
        gKBDEffectMode == KBD_EFFECT_BREATHE)
    {
        kbdStartEffect();
    }
    else
    {
        // Normal hardware brightness control.
        kbdSetHardwareLevel(gKBDLevel[newLevel]);

        if (gKBDEffectsEnabled &&
            gKBDEffectMode == KBD_EFFECT_COLOR_CYCLE)
        {
            kbdStartEffect();
        }
    }
}

bool ClevoService::kbdTerminatedNotificationHandler(
    void *target,
    void *refCon,
    IOService *newService,
    IONotifier *notifier
)
{
    ClevoService *self =
        OSDynamicCast(ClevoService, (OSMetaClassBase *)target);

    if (!self)
    {
        DEBUG_LOG("ClevoService: Terminated Notification handler failed...\n");
        return false;
    }

    if (!self->loggedKeyboards)
    {
        DEBUG_LOG("%s: No Keyboard logged...\n", self->getName());
        return false;
    }

    IOHIKeyboard *keyboard =
        OSDynamicCast(IOHIKeyboard, newService);

    if (!keyboard)
    {
        DEBUG_LOG("%s: Keyboard not found...\n", self->getName());
        return false;
    }

    int index = self->loggedKeyboards->getNextIndexOfObject(keyboard, 0);

    if (index >= 0)
    {
        self->gKBDInUse--;
        self->loggedKeyboards->removeObject(index);

        DEBUG_LOG(
            "%s: Keyboard Successfully Removed %lu\n",
            self->getName(),
            (long)keyboard
        );
    }

    return true;
}


bool ClevoService::kbdNotificationHandler(
    void *target,
    void *refCon,
    IOService *newService,
    IONotifier *notifier
)
{
    ClevoService *self =
        OSDynamicCast(ClevoService, (OSMetaClassBase *)target);

    if (!self)
    {
        DEBUG_LOG("ClevoService: KbdNotification handler failed...\n");
        return false;
    }

    IOHIKeyboard *keyboard =
        OSDynamicCast(IOHIKeyboard, newService);

    if (!keyboard)
    {
        DEBUG_LOG("%s: Keyboard not found...\n", self->getName());
        return false;
    }

    if (!keyboard->_keyboardEventTarget)
    {
        DEBUG_LOG("%s: No Keyboard event target\n", self->getName());
        return false;
    }

    IOService *targetServ =
        OSDynamicCast(IOService, keyboard->_keyboardEventTarget);

    if (targetServ)
    {
        DEBUG_LOG(
            "%s: Keyboard event target is %s\n",
            self->getName(),
            targetServ->getName()
        );
    }

    if (!keyboard->_keyboardEventTarget->metaCast("IOHIDSystem"))
    {
        DEBUG_LOG(
            "%s: IOHIDSystem failed. Continue anyway...\n",
            self->getName()
        );
    }

    int index =
        self->loggedKeyboards->getNextIndexOfObject(keyboard, 0);

    if (index < 0)
    {
        self->loggedKeyboards->setObject(keyboard);
        self->gKBDInUse++;

        DEBUG_LOG(
            "%s::Adding keyboard %lx\n",
            self->getName(),
            (long)keyboard
        );
    }

    origCallback =
        (KeyboardEventCallback)keyboard->_keyboardEventAction;

    keyboard->_keyboardEventAction =
        (KeyboardEventAction)logAction;

    origSpecialCallback =
        (KeyboardSpecialEventCallback)keyboard->_keyboardSpecialEventAction;

    keyboard->_keyboardSpecialEventAction =
        (KeyboardSpecialEventAction)specialAction;

    DEBUG_LOG("%s: Keyboard notification complete\n", self->getName());

    return true;
}


void ClevoService::kbdClearKeyboards()
{
    DEBUG_LOG(
        "%s: Clear Keyboards in use %d!\n",
        getName(),
        gKBDInUse
    );

    if (loggedKeyboards)
    {
        int arraySize = loggedKeyboards->getCount();

        for (int i = 0; i < arraySize; i++)
        {
            IOHIKeyboard *curKeyboard =
                (IOHIKeyboard *)loggedKeyboards->getObject(0);

            if (origSpecialCallback)
            {
                curKeyboard->_keyboardSpecialEventAction =
                    (KeyboardSpecialEventAction)origSpecialCallback;
            }

            if (origCallback)
            {
                curKeyboard->_keyboardEventAction =
                    (KeyboardEventAction)origCallback;
            }

            loggedKeyboards->removeObject(0);
        }
    }

    origSpecialCallback = NULL;
    origCallback = NULL;

    gKBDInUse = 0;
}


void ClevoService::kbdTimerFired(void)
{
    if (!gDimTimerActive)
        return;
    
    if (gKBDIsOFF)
        return;

    if (gKBDJustDim)
    {
        kbdStartFade(gKBDDimmingLevel, true);

        DEBUG_LOG("%s: Keyboard fade-out initiated\n", getName());
    }
    else
    {
        // Stop any currently running fade.
        if (gKBDDimTimer != NULL)
            gKBDDimTimer->cancelTimeout();

        gKBDFading = false;
        gKBDFadeOut = false;

        // Preserve original behavior: set brightness to zero.
        kbdSetHardwareLevel(0);

        gKBDimmed = true;

        DEBUG_LOG("%s: Keyboard automatically dimmed to zero\n", getName());
    }
}


#pragma mark ---------- User Configuration Routines -------------

OSObject* ClevoService::translateEntry(OSObject *obj)
{
    if (OSArray *array = OSDynamicCast(OSArray, obj))
        return translateArray(array);

    if (OSString *string = OSDynamicCast(OSString, obj))
    {
        const char *sz = string->getCStringNoCopy();

        if (sz[0] == '>')
        {
            if (sz[1] == 'y' && !sz[2])
                return OSBoolean::withBoolean(true);

            else if (sz[1] == 'n' && !sz[2])
                return OSBoolean::withBoolean(false);

            // Escape case: '>>n' or '>>y'.
            else if (sz[1] == '>' &&
                     (sz[2] == 'y' || sz[2] == 'n') &&
                     !sz[3])
            {
                return OSString::withCString(&sz[1]);
            }
        }
    }

    return NULL;
}


OSObject* ClevoService::translateArray(OSArray *array)
{
    int count = array->getCount();

    if (!count)
        return NULL;

    OSObject *result = array;

    OSArray *test =
        OSDynamicCast(OSArray, array->getObject(0));

    if (test && test->getCount() == 0)
    {
        array->retain();
        array->removeObject(0);

        --count;

        for (int i = 0; i < count; ++i)
        {
            if (OSObject *obj = translateEntry(array->getObject(i)))
            {
                array->replaceObject(i, obj);
                obj->release();
            }
        }
    }
    else
    {
        if (count & 1)
            return NULL;

        int size = count >> 1;

        if (!size)
            size = 1;

        OSDictionary *dict =
            OSDictionary::withCapacity(size);

        if (!dict)
            return NULL;

        for (int i = 0; i < count; i += 2)
        {
            OSString *key =
                OSDynamicCast(OSString, array->getObject(i));

            if (!key)
            {
                dict->release();
                return NULL;
            }

            OSObject *obj = array->getObject(i + 1);
            OSObject *trans = translateEntry(obj);

            if (trans)
                obj = trans;

            dict->setObject(key, obj);

            OSSafeReleaseNULL(trans);
        }

        result = dict;
    }

    return result;
}


void ClevoService::loadConfiguration()
{
    OSObject *reply = NULL;

    if (kIOReturnSuccess !=
        device->evaluateObject(ACPI_CONF_NAME, &reply))
    {
        DEBUG_LOG(
            "%s: SSDT Configuration Method not found...\n",
            getName()
        );

        return;
    }

    OSObject *obj = NULL;

    OSArray *array =
        OSDynamicCast(OSArray, reply);

    if (array)
        obj = translateArray(array);

    OSSafeReleaseNULL(reply);

    OSDictionary *config =
        OSDynamicCast(OSDictionary, obj);

    if (!config)
    {
        DEBUG_LOG(
            "%s: unable to load Configuration data...\n",
            getName()
        );

        OSSafeReleaseNULL(obj);
        return;
    }

    OSBoolean *osBool =
        OSDynamicCast(
            OSBoolean,
            config->getObject("KbdAutoDimTimerActive")
        );

    if (osBool)
        gDimTimerActive = osBool->isTrue();

    osBool =
        OSDynamicCast(
            OSBoolean,
            config->getObject("KbdAutoDimActive")
        );

    if (osBool)
        gKBDJustDim = osBool->isTrue();

    // Load master effects switch.
    osBool = OSDynamicCast(
        OSBoolean,
        config->getObject("KbdEffectsActive")
    );

    if (osBool)
        gKBDEffectsEnabled = osBool->isTrue();
    else
        gKBDEffectsEnabled = false;

    // Load selected effect mode.
    OSNumber *effectMode = OSDynamicCast(
        OSNumber,
        config->getObject("KbdEffectMode")
    );

    if (effectMode)
    {
        UInt32 mode = effectMode->unsigned32BitValue();

        if (mode <= KBD_EFFECT_COLOR_CYCLE)
            gKBDEffectMode = (KBDEffectMode)mode;
    }
    else
    {
        gKBDEffectMode = KBD_EFFECT_NONE;
    }

    // Load the Breathe color mode.
    OSNumber *breatheColorMode = OSDynamicCast(
        OSNumber,
        config->getObject("KbdBreatheColorMode")
    );

    if (breatheColorMode)
    {
        UInt32 mode = breatheColorMode->unsigned32BitValue();

        if (mode <= KBD_BREATHE_MULTI_COLOR)
            gKBDBreatheColorMode = (KBDBreatheColorMode)mode;
    }

    OSNumber *osNum =
        OSDynamicCast(
            OSNumber,
            config->getObject("KbdAutoDimTime")
        );

    if (osNum)
    {
        gKBDTime = osNum->unsigned32BitValue();

        if (gKBDTime < 5)
            gKBDTime = 5;
        else if (gKBDTime > 1800)
            gKBDTime = 1800;

        // Configuration is in seconds; timer expects milliseconds.
        gKBDTime *= 1000;
    }

    if (OSArray *levelsArray =
        OSDynamicCast(OSArray, config->getObject("KbdLevels")))
    {
        if (levelsArray->getCount() == 4)
        {
            for (UInt8 i = 0; i < 4; i++)
            {
                osNum =
                    OSDynamicCast(OSNumber, levelsArray->getObject(i));

                if (osNum && osNum->unsigned8BitValue() > 0)
                {
                    gKBDLevel[i] =
                        osNum->unsigned8BitValue();
                }
            }
        }
    }
    else
    {
        DEBUG_LOG("%s: KbdLevels array not found\n", getName());
    }

    osNum =
        OSDynamicCast(
            OSNumber,
            config->getObject("KbdDimmingLevel")
        );

    if (osNum)
        gKBDDimmingLevel = osNum->unsigned8BitValue() & 0xF;

    OSSafeReleaseNULL(config);

    DEBUG_LOG("%s: Configuration Successfully Loaded\n", getName());
}


#pragma mark ---------- Keyboard Logger Actions -------------

void specialAction(
    OSObject *target,
    unsigned eventType,
    unsigned flags,
    unsigned key,
    unsigned flavor,
    UInt64 guid,
    bool repeat,
    AbsoluteTime ts,
    OSObject *sender,
    void *refcon __unused
)
{
    DEBUG_LOG(
        "%s: SpecialAction called with eventType %d\n",
        gKBDService->getName(),
        eventType
    );

    // Only sign of a logout (also thrown when sleeping).
    if ((eventType == NX_SYSDEFINED) &&
        (!flags) &&
        (key == NX_NOSPECIALKEY))
    {
        gKBDService->kbdClearKeyboards();
    }

    if (origSpecialCallback)
    {
        (*origSpecialCallback)(
            target,
            eventType,
            flags,
            key,
            flavor,
            guid,
            repeat,
            ts,
            sender,
            0
        );
    }
}


void logAction(
    OSObject *target,
    unsigned eventType,
    unsigned flags,
    unsigned key,
    unsigned charCode,
    unsigned charSet,
    unsigned origCharCode,
    unsigned origCharSet,
    unsigned keyboardType,
    bool repeat,
    AbsoluteTime ts,
    OSObject *sender,
    void *refcon __unused
)
{
    DEBUG_LOG(
        "%s: LogAction called with eventType %d\n",
        gKBDService->getName(),
        eventType
    );

    bool kbdEvtFlag =
        (eventType == NX_KEYDOWN) ||
        (eventType == NX_FLAGSCHANGED);

    if (gDimTimerActive &&
        !gKBDIsOFF &&
        kbdEvtFlag &&
        gKBDService)
    {
        if (gKBDTimer != NULL)
        {
            gKBDTimer->cancelTimeout();
            gKBDTimer->setTimeoutMS(gKBDTime);

            DEBUG_LOG(
                "%s: Key Down, Timer Reloaded %d mS\n",
                gKBDService->getName(),
                gKBDTime
            );

            gKBDService->kbdRestoreBacklight();
        }
    }

    if (origCallback)
    {
        (*origCallback)(
            target,
            eventType,
            flags,
            key,
            charCode,
            charSet,
            origCharCode,
            origCharSet,
            keyboardType,
            repeat,
            ts,
            sender,
            0
        );
    }
}
