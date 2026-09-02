// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The bridge between the QML interface and the two things it touches: the
// settings file, and the live state the daemon publishes.
//
// A property write edits the window's own copy; Apply writes the file. The overlay
// notices within two seconds because it polls the file's timestamp, so the game
// follows a moment after Apply and nobody is ever told to restart anything.
//
// The exceptions are the three switches that turn drawing on and off, which are
// reachable from the tray as well as the window and take effect at once: see
// persistNow().

#ifndef VOCEM_CONFIG_BRIDGE_H
#define VOCEM_CONFIG_BRIDGE_H

#include <QColor>
#include <QFont>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QProcess;

#include "desktop_entries.h"

#include "vocem/apps.h"
#include "vocem/config.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"

class ConfigBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    // What the window switches on. The status sentence is for reading, not for
    // comparing: three places used to test it against tr("Waiting") and
    // tr("Not running"), which quietly stopped being true in every language but
    // this one -- the pulsing halo went still and the state dot went amber where
    // it should have been red.
    enum State { NotRunning, Waiting, Working, Refused, Connected };
    Q_ENUM(State)

    // The user's own voice state, for the tray icon: the segment carries a
    // self flag per participant (the daemon sets it from Discord's is_self),
    // so the icon can be the speaking ring instead of a static picture.
    // Deafened wins over muted, which is the client's own precedence.
    enum SelfVoice { NotInChannel, InChannelIdle, Speaking, Muted, Deafened };
    Q_ENUM(SelfVoice)

private:

    Q_PROPERTY(qreal positionX READ positionX WRITE setPositionX NOTIFY configChanged)
    Q_PROPERTY(qreal positionY READ positionY WRITE setPositionY NOTIFY configChanged)
    Q_PROPERTY(qreal scale READ scale WRITE setScale NOTIFY configChanged)
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY configChanged)
    // Colours as QML colour values. Stored as 0xRRGGBB, with transparency kept
    // separate so changing one never disturbs the other.
    Q_PROPERTY(QColor panelColour READ panelColour WRITE setPanelColour NOTIFY configChanged)
    Q_PROPERTY(QColor notificationColour READ notificationColour WRITE setNotificationColour
                   NOTIFY configChanged)
    Q_PROPERTY(QColor speakingColour READ speakingColour WRITE setSpeakingColour
                   NOTIFY configChanged)
    // The three text colours a user may pin, as the file spells them: "auto" --
    // the default, following the box's ramp -- or "#rrggbb". Strings rather than
    // colours because "auto" is not a colour, and because the page's Reset
    // compares a setting against its default as text: an invalid QColor standing
    // for auto would print as black and make a pinned black read as unchanged.
    // The effective* companions are what theme_for() actually draws -- the ramp
    // while auto, the pinned value (nudged a step where it would collide with
    // another measured role; see theme.h) once pinned -- and they are what the
    // swatch shows, so the reset always reveals the colour auto stands for.
    Q_PROPERTY(QString textIdleColour READ textIdleColour WRITE setTextIdleColour
                   NOTIFY configChanged)
    Q_PROPERTY(QString textSpeakingColour READ textSpeakingColour WRITE setTextSpeakingColour
                   NOTIFY configChanged)
    Q_PROPERTY(QString notificationTextColour READ notificationTextColour
                   WRITE setNotificationTextColour NOTIFY configChanged)
    Q_PROPERTY(QString defaultTextIdleColour READ defaultTextIdleColour CONSTANT)
    Q_PROPERTY(QString defaultTextSpeakingColour READ defaultTextSpeakingColour CONSTANT)
    Q_PROPERTY(QString defaultNotificationTextColour READ defaultNotificationTextColour CONSTANT)
    Q_PROPERTY(QColor effectiveTextIdleColour READ effectiveTextIdleColour NOTIFY configChanged)
    Q_PROPERTY(QColor effectiveTextSpeakingColour READ effectiveTextSpeakingColour
                   NOTIFY configChanged)
    Q_PROPERTY(QColor effectiveNotificationTextColour READ effectiveNotificationTextColour
                   NOTIFY configChanged)
    Q_PROPERTY(bool panelEnabled READ panelEnabled WRITE setPanelEnabled NOTIFY configChanged)
    // 0 upright, 1 sideways -- the numbers Config keeps, so the page's combo box
    // index and the setting are the same value and nothing translates between
    // them. The word is only ever written to the file (Config::layout_text).
    Q_PROPERTY(int panelLayout READ panelLayout WRITE setPanelLayout NOTIFY configChanged)
    Q_PROPERTY(int defaultPanelLayout READ defaultPanelLayout CONSTANT)
    // 0 the whole panel, 1 behind the names -- Config's own numbers again, for
    // the same reason: the combo box's index and the setting are one value, and
    // the word is only ever written to the file (Config::box_text).
    Q_PROPERTY(int panelBox READ panelBox WRITE setPanelBox NOTIFY configChanged)
    Q_PROPERTY(int defaultPanelBox READ defaultPanelBox CONSTANT)
    Q_PROPERTY(qreal notificationScale READ notificationScale WRITE setNotificationScale NOTIFY configChanged)
    Q_PROPERTY(qreal screenMargin READ screenMargin WRITE setScreenMargin NOTIFY configChanged)
    // The message's own distance from the edge: the panel's setting moved a box
    // whose owner had not asked it to move.
    Q_PROPERTY(qreal notificationMargin READ notificationMargin WRITE setNotificationMargin
                   NOTIFY configChanged)
    Q_PROPERTY(qreal boxPaddingX READ boxPaddingX WRITE setBoxPaddingX NOTIFY configChanged)
    Q_PROPERTY(qreal boxPaddingY READ boxPaddingY WRITE setBoxPaddingY NOTIFY configChanged)
    Q_PROPERTY(qreal avatarGap READ avatarGap WRITE setAvatarGap NOTIFY configChanged)
    Q_PROPERTY(qreal rowSpacing READ rowSpacing WRITE setRowSpacing NOTIFY configChanged)
    // Whether the background has been turned down far enough to disappear in game.
    // A legitimate setting, but the interface has to say so: an opacity that reached
    // zero by accident is indistinguishable from a broken overlay.
    Q_PROPERTY(bool backgroundFaint READ backgroundFaint NOTIFY configChanged)
    Q_PROPERTY(bool notificationBackgroundFaint READ notificationBackgroundFaint
                   NOTIFY configChanged)
    // What a reset goes back to, from the same defaults the overlay starts from
    // rather than from a number written twice. Every slider has one: the reset is
    // part of the row rather than something four of the six rows went without.
    Q_PROPERTY(qreal defaultOpacity READ defaultOpacity CONSTANT)
    Q_PROPERTY(qreal defaultScale READ defaultScale CONSTANT)
    Q_PROPERTY(qreal defaultNotificationScale READ defaultNotificationScale CONSTANT)
    Q_PROPERTY(qreal defaultAvatarSize READ defaultAvatarSize CONSTANT)
    Q_PROPERTY(qreal defaultNotificationOpacity READ defaultNotificationOpacity CONSTANT)
    Q_PROPERTY(qreal defaultNotificationSeconds READ defaultNotificationSeconds CONSTANT)
    Q_PROPERTY(qreal defaultScreenMargin READ defaultScreenMargin CONSTANT)
    Q_PROPERTY(qreal defaultNotificationMargin READ defaultNotificationMargin CONSTANT)
    Q_PROPERTY(qreal defaultBoxPaddingX READ defaultBoxPaddingX CONSTANT)
    Q_PROPERTY(qreal defaultBoxPaddingY READ defaultBoxPaddingY CONSTANT)
    Q_PROPERTY(qreal defaultAvatarGap READ defaultAvatarGap CONSTANT)
    Q_PROPERTY(qreal defaultRowSpacing READ defaultRowSpacing CONSTANT)
    Q_PROPERTY(qreal defaultPositionX READ defaultPositionX CONSTANT)
    Q_PROPERTY(qreal defaultPositionY READ defaultPositionY CONSTANT)
    Q_PROPERTY(int defaultNotificationCorner READ defaultNotificationCorner CONSTANT)
    Q_PROPERTY(bool defaultShowChannelName READ defaultShowChannelName CONSTANT)
    Q_PROPERTY(bool defaultOnlySpeaking READ defaultOnlySpeaking CONSTANT)
    Q_PROPERTY(bool defaultHideSelf READ defaultHideSelf CONSTANT)
    Q_PROPERTY(bool defaultShowMutedState READ defaultShowMutedState CONSTANT)
    Q_PROPERTY(QColor defaultPanelColour READ defaultPanelColour CONSTANT)
    Q_PROPERTY(QColor defaultNotificationColour READ defaultNotificationColour CONSTANT)
    Q_PROPERTY(QColor defaultSpeakingColour READ defaultSpeakingColour CONSTANT)
    Q_PROPERTY(qreal avatarSize READ avatarSize WRITE setAvatarSize NOTIFY configChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY configChanged)
    Q_PROPERTY(qreal defaultFontSize READ defaultFontSize CONSTANT)
    // The typeface. Empty is the carried Inter, which is what the overlay drew
    // before this existed and what it falls back to whenever the chosen file
    // cannot be used. The list is what this machine has that the overlay can
    // rasterise -- TrueType outlines only -- so the window never offers a font
    // the game would refuse.
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY configChanged)
    Q_PROPERTY(QString defaultFontFamily READ defaultFontFamily CONSTANT)
    // The carried Inter, by the name Qt knows it under. Separate from
    // overlayFont(): that one answers "what should this preview be drawn in",
    // which starts with the chosen family, and the row that offers to go *back*
    // to the built-in font has to be drawn in the built-in font.
    Q_PROPERTY(QString builtInFontFamily READ builtInFontFamily CONSTANT)
    Q_PROPERTY(bool textShadow READ textShadow WRITE setTextShadow NOTIFY configChanged)
    Q_PROPERTY(bool defaultTextShadow READ defaultTextShadow CONSTANT)
    Q_PROPERTY(bool showChannelName READ showChannelName WRITE setShowChannelName NOTIFY configChanged)
    Q_PROPERTY(bool onlySpeaking READ onlySpeaking WRITE setOnlySpeaking NOTIFY configChanged)
    Q_PROPERTY(bool hideSelf READ hideSelf WRITE setHideSelf NOTIFY configChanged)
    Q_PROPERTY(bool showMutedState READ showMutedState WRITE setShowMutedState NOTIFY configChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY configChanged)
    // What the window does with itself: see the note in config.h.
    Q_PROPERTY(bool keepRunning READ keepRunning WRITE setKeepRunning NOTIFY configChanged)
    Q_PROPERTY(bool defaultKeepRunning READ defaultKeepRunning CONSTANT)
    Q_PROPERTY(bool trayVoiceIcon READ trayVoiceIcon WRITE setTrayVoiceIcon NOTIFY configChanged)
    Q_PROPERTY(bool defaultTrayVoiceIcon READ defaultTrayVoiceIcon CONSTANT)
    // What the tray icon is actually wearing, which is the SAVED answer and not
    // the edited one. Every other setting may be previewed before Apply because
    // what it changes is a picture inside this window; this one changes a thing
    // on the user's panel, and a radio button that reached out and altered the
    // desktop before Apply -- and left it altered if the window were closed
    // without applying -- would be the one control here that cannot be tried
    // out. The page's own preview follows the edit; the panel follows the file.
    Q_PROPERTY(bool appliedTrayVoiceIcon READ appliedTrayVoiceIcon NOTIFY configChanged)
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY configChanged)
    Q_PROPERTY(bool defaultStartAtLogin READ defaultStartAtLogin CONSTANT)
    // The applications the overlay stays out of, as the list the file carries.
    // Exposed as a whole so the page's Reset can put it back to the default in one
    // assignment, like every other setting; the switches go through
    // setApplicationHidden() rather than editing the string themselves.
    Q_PROPERTY(QString hiddenApps READ hiddenApps WRITE setHiddenApps NOTIFY configChanged)
    Q_PROPERTY(QString defaultHiddenApps READ defaultHiddenApps CONSTANT)
    Q_PROPERTY(QString shownApps READ shownApps WRITE setShownApps NOTIFY configChanged)
    Q_PROPERTY(QString defaultShownApps READ defaultShownApps CONSTANT)
    // Every application the overlay has been loaded into, newest first. Not a
    // setting: a record written by the injected code, which is why it is refreshed
    // on the timer rather than emitted by an edit.
    Q_PROPERTY(QVariantList applications READ applications NOTIFY applicationsChanged)
    // The overlay's live instances: which processes are drawing it *right now*,
    // from the journals the injected code opens at its first drawn frame
    // (vocem/journal.h). The Applications page shows them above everything
    // else, because a list of every application the overlay has ever been loaded
    // into could not say which of them has it on screen at this moment -- and that
    // is the first thing somebody looking at that page wants to know.
    Q_PROPERTY(QVariantList liveInstances READ liveInstances NOTIFY liveInstancesChanged)
    // What the Debug section reads. The journals whose process ended without
    // unwinding -- a crash, or a forced stop -- each with its full text; the
    // finished sessions the journal keeps as history; and the two halves of
    // the ABI question, because a reader that refuses a segment from another
    // ABI looks exactly like "no daemon" from outside and the difference has
    // to be SAYABLE somewhere (entry 55 is what that silence costs).
    Q_PROPERTY(QVariantList crashReports READ crashReports NOTIFY crashReportsChanged)
    Q_PROPERTY(QVariantList journalHistory READ journalHistory NOTIFY journalChanged)
    Q_PROPERTY(int abiVersion READ abiVersion CONSTANT)
    Q_PROPERTY(int segmentAbiVersion READ segmentAbiVersion NOTIFY stateChanged)
    // The daemon's journald lines, filled by refreshDaemonLog(): running
    // journalctl on a half-second timer would be absurd, so the Debug page
    // asks when it opens and when its refresh button is pressed.
    Q_PROPERTY(QString daemonLog READ daemonLog NOTIFY journalChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled
                   NOTIFY configChanged)
    Q_PROPERTY(int notificationCorner READ notificationCorner WRITE setNotificationCorner
                   NOTIFY configChanged)
    Q_PROPERTY(qreal notificationSeconds READ notificationSeconds WRITE setNotificationSeconds
                   NOTIFY configChanged)
    Q_PROPERTY(qreal notificationOpacity READ notificationOpacity WRITE setNotificationOpacity
                   NOTIFY configChanged)
    // Every colour and proportion the overlay draws with, from the same file the
    // injected code reads: include/vocem/theme.h. The previews used to carry their
    // own copy of each of them, written by hand, which meant a colour changed in
    // the drawing code and left unchanged here produced a preview that was
    // confidently wrong -- and there is no numeric comparison for a colour the way
    // there is for a distance, so nothing would have said so.
    //
    // A map rather than a set of properties because there are dozens of them and
    // they are read, never written. The cost is that a mistyped key is `undefined`
    // rather than an error, which is why the previews assert the key set they
    // expect when they are created.
    Q_PROPERTY(QVariantMap overlayTheme READ overlayTheme NOTIFY configChanged)
    // The ready-made surfaces on the Appearance page, from kPresets in theme.h --
    // the same table tests/theme_contrast.cpp holds to the contrast floor, so the
    // row cannot offer a surface the measurement does not cover. Each entry is
    // {id, colour, opacity}; the words shown beside the swatch are the window's.
    Q_PROPERTY(QVariantList overlayPresets READ overlayPresets CONSTANT)
    // One theme per preset, in overlayPresets' order: the window's edited copy
    // of the settings with that preset's two writes applied -- the surface and
    // the opacity, exactly what clicking the preset writes -- passed through
    // the same theme_for() as overlayTheme. The Appearance page's preset
    // previews draw from these, so a preset is shown by the arithmetic that
    // will draw it rather than by a swatch mixed in QML. Not constant: every
    // other setting passes through, so the previews follow the controls live.
    Q_PROPERTY(QVariantList presetThemes READ presetThemes NOTIFY configChanged)
    // What the previews draw. Fixed, and never anybody real: see the note above
    // participants() for why the live channel was the wrong thing to show. Not
    // constant: the example is rebuilt with the settings, and its body is
    // always there, because the drawn toast always carries one.
    Q_PROPERTY(QVariantMap notificationPreview READ notificationPreview NOTIFY configChanged)
    Q_PROPERTY(QString channelName READ channelName CONSTANT)
    Q_PROPERTY(QVariantList participants READ participants CONSTANT)

    // Whether an edit is waiting to be written. What the Apply button is enabled
    // by, and the reason there is one.
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)

    // Live state, refreshed on a timer.
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    // Its own signal, emitted only when the value moves: the tray re-sets its
    // icon on this, and re-setting an unchanged icon twice a second would be
    // D-Bus traffic for nothing.
    Q_PROPERTY(SelfVoice selfVoice READ selfVoice NOTIFY selfVoiceChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    // What the button should offer right now, and whether to offer one at all.
    Q_PROPERTY(QString actionText READ actionText NOTIFY stateChanged)
    Q_PROPERTY(bool actionAvailable READ actionAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString hintText READ hintText NOTIFY stateChanged)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    // The resolution of the display, for the caption on the maps of it. Read from
    // the kernel rather than from Qt -- see the note on the definition. Not
    // constant: a display that is asleep answers nothing, and the next tick asks
    // again.
    Q_PROPERTY(QString screenResolution READ screenResolution NOTIFY stateChanged)
    // Every connected display, as {name, width, height}, from the same kernel
    // enumeration the caption reads (environment.h) -- so the dropdown beside a
    // map and the caption inside it cannot disagree. On the caption's cadence
    // for the caption's reason: a display asleep at startup answers nothing and
    // is asked again.
    Q_PROPERTY(QVariantList displays READ displays NOTIFY stateChanged)
    // The display height the overlay actually sizes itself from: the largest
    // connected mode, the same number the daemon publishes (vocem/display.h).
    // What lets a map depicting a smaller display say, honestly, how much
    // larger the overlay will look there. Zero where no mode is readable.
    Q_PROPERTY(int overlayDisplayHeight READ overlayDisplayHeight NOTIFY stateChanged)
    // The shape of that display, as one number, and zero where no mode can be
    // read. A map with no display chosen stands for the display the overlay is
    // sized for, so it takes its shape from here rather than from `Screen` --
    // the screen this window happens to be sitting on, which is a different
    // display on any machine with two. One ratio and not the two sides of it:
    // a number compares equal from one tick to the next, where a map of
    // {width, height} is a fresh object every time this is read and would
    // relayout both previews twice a second.
    Q_PROPERTY(qreal sizingDisplayAspect READ sizingDisplayAspect NOTIFY stateChanged)
    // Which display each map depicts, by connector name; empty means automatic
    // (the largest, which is what the overlay is sized for). Persisted like any
    // other setting: an edit waits for Apply.
    Q_PROPERTY(QString panelPreviewDisplay READ panelPreviewDisplay
                   WRITE setPanelPreviewDisplay NOTIFY configChanged)
    Q_PROPERTY(QString defaultPanelPreviewDisplay READ defaultPanelPreviewDisplay CONSTANT)
    Q_PROPERTY(QString notificationPreviewDisplay READ notificationPreviewDisplay
                   WRITE setNotificationPreviewDisplay NOTIFY configChanged)
    Q_PROPERTY(QString defaultNotificationPreviewDisplay READ defaultNotificationPreviewDisplay
                   CONSTANT)
    // The shape of the display the maps stand for, when it has been pinned rather
    // than discovered. Zero -- the normal case -- means the map asks Qt, as it
    // always has.
    //
    // It exists for the comparison harness. The maps take their aspect from
    // `Screen`, which is the logical size of whatever screen the window happens to
    // be on: it is not the same number on two developers' machines, and under a
    // platform plugin with no real screen it is not a display shape at all. That
    // makes "the panel's share of the screen" a figure about the machine the check
    // ran on rather than about the drawing, which is the one thing a numeric
    // comparison must not be.
    Q_PROPERTY(qreal pinnedScreenAspect READ pinnedScreenAspect CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    // Whether the two injection paths are actually in place. The overlay is
    // invisible when it is not working, so the one thing About can usefully do is
    // say whether the parts that have to be installed are installed -- rather than
    // leaving someone to guess why nothing appears in their game.
    Q_PROPERTY(bool vulkanLayerInstalled READ vulkanLayerInstalled CONSTANT)
    Q_PROPERTY(bool openglPreloadActive READ openglPreloadActive CONSTANT)
    // The overlay's own typeface, and the correction that makes Qt draw it at the
    // size ImGui would. Both previews use these, so what they show is as wide as
    // what the game draws.
    // Not a family name but a whole font, because there are three families and a
    // QML `font` has room for one. The letters come from Inter, the emoji and the
    // CJK punctuation from the two fonts merged beside it in the atlas, and
    // QFont::setFamilies is how Qt is told the same thing: in order, the first one
    // that has the character wins.
    // Follows the chosen family: it is a property of the font file, and the file
    // is a setting now. CONSTANT here would have left every preview laying out
    // somebody else's letters against Inter's proportion.
    Q_PROPERTY(qreal overlayFontRatio READ overlayFontRatio NOTIFY configChanged)

public:
    explicit ConfigBridge(QObject* parent = nullptr);

    qreal positionX() const { return config_.position_x; }
    qreal positionY() const { return config_.position_y; }
    qreal scale() const { return config_.scale; }
    qreal opacity() const { return config_.opacity; }
    // Component-wise, not QColor::fromRgb(uint): that overload reads its argument
    // as 0xAARRGGBB, so a plain 0xRRGGBB arrives with an alpha of zero and every
    // box came out invisible.
    static QColor toColour(uint32_t rgb) {
        return QColor::fromRgb((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    }
    QColor panelColour() const { return toColour(config_.panel_colour); }
    QColor notificationColour() const { return toColour(config_.notification_colour); }
    QColor speakingColour() const { return toColour(config_.speaking_colour); }
    // "auto" or "#rrggbb" -- the same two spellings the file takes.
    static QString colourOrAuto(uint32_t value) {
        return value == vocem::Config::kColourAuto ? QStringLiteral("auto")
                                                   : toColour(value).name();
    }
    QString textIdleColour() const { return colourOrAuto(config_.text_idle_colour); }
    QString textSpeakingColour() const { return colourOrAuto(config_.text_speaking_colour); }
    QString notificationTextColour() const {
        return colourOrAuto(config_.notification_text_colour);
    }
    QString defaultTextIdleColour() const { return QStringLiteral("auto"); }
    QString defaultTextSpeakingColour() const { return QStringLiteral("auto"); }
    QString defaultNotificationTextColour() const { return QStringLiteral("auto"); }
    QColor effectiveTextIdleColour() const;
    QColor effectiveTextSpeakingColour() const;
    QColor effectiveNotificationTextColour() const;
    bool panelEnabled() const { return config_.panel_enabled; }
    int panelLayout() const { return config_.panel_layout; }
    int defaultPanelLayout() const { return vocem::Config{}.panel_layout; }
    int panelBox() const { return config_.panel_box; }
    int defaultPanelBox() const { return vocem::Config{}.panel_box; }
    qreal notificationScale() const { return config_.notification_scale; }
    qreal screenMargin() const { return config_.screen_margin; }
    qreal notificationMargin() const { return config_.notification_margin; }
    qreal boxPaddingX() const { return config_.box_padding_x; }
    qreal boxPaddingY() const { return config_.box_padding_y; }
    qreal avatarGap() const { return config_.avatar_gap; }
    qreal rowSpacing() const { return config_.row_spacing; }
    bool backgroundFaint() const { return config_.background_is_faint(); }
    bool notificationBackgroundFaint() const {
        return config_.notification_background_is_faint();
    }
    qreal defaultOpacity() const { return vocem::Config{}.opacity; }
    qreal defaultScale() const { return vocem::Config{}.scale; }
    qreal defaultNotificationScale() const { return vocem::Config{}.notification_scale; }
    qreal defaultAvatarSize() const { return vocem::Config{}.avatar_size; }
    qreal defaultNotificationOpacity() const { return vocem::Config{}.notification_opacity; }
    qreal defaultNotificationSeconds() const { return vocem::Config{}.notification_seconds; }
    qreal defaultScreenMargin() const { return vocem::Config{}.screen_margin; }
    qreal defaultNotificationMargin() const { return vocem::Config{}.notification_margin; }
    qreal defaultBoxPaddingX() const { return vocem::Config{}.box_padding_x; }
    qreal defaultBoxPaddingY() const { return vocem::Config{}.box_padding_y; }
    qreal defaultAvatarGap() const { return vocem::Config{}.avatar_gap; }
    qreal defaultRowSpacing() const { return vocem::Config{}.row_spacing; }
    qreal defaultPositionX() const { return vocem::Config{}.position_x; }
    qreal defaultPositionY() const { return vocem::Config{}.position_y; }
    int defaultNotificationCorner() const { return vocem::Config{}.notification_corner; }
    bool defaultShowChannelName() const { return vocem::Config{}.show_channel_name; }
    bool defaultOnlySpeaking() const { return vocem::Config{}.only_speaking; }
    bool defaultHideSelf() const { return vocem::Config{}.hide_self; }
    bool defaultShowMutedState() const { return vocem::Config{}.show_muted_state; }
    QColor defaultPanelColour() const { return toColour(vocem::Config{}.panel_colour); }
    QColor defaultNotificationColour() const {
        return toColour(vocem::Config{}.notification_colour);
    }
    QColor defaultSpeakingColour() const { return toColour(vocem::Config{}.speaking_colour); }
    qreal avatarSize() const { return config_.avatar_size; }
    qreal fontSize() const { return config_.font_size; }
    qreal defaultFontSize() const { return vocem::Config{}.font_size; }
    QString fontFamily() const { return QString::fromStdString(config_.font_family); }
    QString defaultFontFamily() const {
        return QString::fromStdString(vocem::Config{}.font_family);
    }
    // Every family this machine has that the overlay can rasterise. Defined
    // beside the rest of the machine-facing code in the .cpp: this header keeps
    // the settings, and what fonts a machine has is not one.
    //
    // A function and not a property, which is a performance contract and not a
    // style: a QStringList reaching QML as a *property* is wrapped in a
    // reference sequence, and every indexed read of it calls this getter again
    // and converts the whole list. `[""].concat(config.fontFamilies)` therefore
    // asked for the list once per family -- 272 reads of a 272-name list,
    // measured at 0.95 s of this window's startup on a machine with 271
    // families, paid before the first frame whether or not anybody ever opens
    // the Appearance page. Returned from an invokable the same list is
    // converted once (measured: 1 call).
    Q_INVOKABLE QStringList fontFamilies() const;
    QString builtInFontFamily() const;
    bool textShadow() const { return config_.text_shadow; }
    bool defaultTextShadow() const { return vocem::Config{}.text_shadow; }
    bool showChannelName() const { return config_.show_channel_name; }
    bool onlySpeaking() const { return config_.only_speaking; }
    bool hideSelf() const { return config_.hide_self; }
    bool showMutedState() const { return config_.show_muted_state; }
    bool enabled() const { return config_.enabled; }
    bool keepRunning() const { return config_.keep_running; }
    bool defaultKeepRunning() const { return vocem::Config{}.keep_running; }
    bool trayVoiceIcon() const { return config_.tray_voice_icon; }
    bool defaultTrayVoiceIcon() const { return vocem::Config{}.tray_voice_icon; }
    bool appliedTrayVoiceIcon() const { return saved_.tray_voice_icon; }
    bool startAtLogin() const { return config_.start_at_login; }
    bool defaultStartAtLogin() const { return vocem::Config{}.start_at_login; }
    void setKeepRunning(bool value);
    void setTrayVoiceIcon(bool value);
    void setStartAtLogin(bool value);
    QString hiddenApps() const { return QString::fromStdString(config_.hidden_apps); }
    QString defaultHiddenApps() const {
        return QString::fromStdString(vocem::Config{}.hidden_apps);
    }
    void setHiddenApps(const QString& value);
    QString shownApps() const { return QString::fromStdString(config_.shown_apps); }
    QString defaultShownApps() const { return QString::fromStdString(vocem::Config{}.shown_apps); }
    void setShownApps(const QString& value);
    QVariantList applications() const { return applications_; }
    // The switch beside one application: whether the overlay draws there. Which
    // of the two lists it lands in depends on what the detection made of it, which
    // is why that answer is passed back in.
    Q_INVOKABLE void setApplicationDrawn(const QString& name, bool drawn, bool game);
    // The list is a history: something uninstalled a year ago has no business
    // still being in it. Not a setting, so it takes effect at once.
    Q_INVOKABLE void forgetApplications();
    bool notificationsEnabled() const { return config_.notifications_enabled; }
    int notificationCorner() const { return config_.notification_corner; }
    qreal notificationSeconds() const { return config_.notification_seconds; }
    qreal notificationOpacity() const { return config_.notification_opacity; }
    QVariantMap overlayTheme() const;
    QVariantList overlayPresets() const;
    QVariantList presetThemes() const;
    QVariantMap notificationPreview() const;

    void setPositionX(qreal value);
    void setPositionY(qreal value);

    // Both at once, so dragging writes the file once per move instead of twice.
    Q_INVOKABLE void setPosition(qreal x, qreal y);
    void setScale(qreal value);
    void setOpacity(qreal value);
    void setPanelColour(const QColor& value);
    void setNotificationColour(const QColor& value);
    void setSpeakingColour(const QColor& value);
    void setTextIdleColour(const QString& value);
    void setTextSpeakingColour(const QString& value);
    void setNotificationTextColour(const QString& value);
    void setPanelEnabled(bool value);
    void setPanelLayout(int value);
    void setPanelBox(int value);
    void setNotificationScale(qreal value);
    void setScreenMargin(qreal value);
    void setNotificationMargin(qreal value);
    void setBoxPaddingX(qreal value);
    void setBoxPaddingY(qreal value);
    void setAvatarGap(qreal value);
    void setRowSpacing(qreal value);
    void setAvatarSize(qreal value);
    void setFontSize(qreal value);
    // Writes the family *and* the two files it resolves to: the game cannot ask
    // fontconfig anything, so the window has to hand it paths.
    void setFontFamily(const QString& value);
    void setTextShadow(bool value);
    void setShowChannelName(bool value);
    void setOnlySpeaking(bool value);
    void setHideSelf(bool value);
    void setShowMutedState(bool value);
    void setEnabled(bool value);
    void setNotificationsEnabled(bool value);
    void setNotificationCorner(int value);
    void setNotificationSeconds(qreal value);
    void setNotificationOpacity(qreal value);

    State state() const;
    SelfVoice selfVoice() const { return self_voice_; }
    QString statusText() const;
    QString actionText() const;
    QString hintText() const;
    bool actionAvailable() const;

    // The one button. Starts the daemon when it is not running, and asks for
    // authorisation again when Discord refused or the token went stale.
    bool pending() const { return pending_; }
    // Write the edits to the file, where the daemon and any running game will pick
    // them up within two seconds.
    Q_INVOKABLE void apply();

    Q_INVOKABLE void performAction();
    Q_INVOKABLE void reauthorise();
    // Quit, meaning quit: stops the daemon -- which takes the overlay out of
    // every running game within a second -- and then ends this process. The
    // tray's Quit and the close button outside the keep-running case both come
    // here; the application's own start is the matching half, which is what
    // makes "reopen and it comes back" true.
    Q_INVOKABLE void quitOverlay();
    QString channelName() const;
    QVariantList participants() const;
    QString configPath() const { return QString::fromStdString(vocem::Config::path()); }
    QString screenResolution() const;
    QVariantList displays() const;
    int overlayDisplayHeight() const;
    qreal sizingDisplayAspect() const;
    QString panelPreviewDisplay() const {
        return QString::fromStdString(config_.preview_display_panel);
    }
    QString defaultPanelPreviewDisplay() const { return QString(); }
    void setPanelPreviewDisplay(const QString& value);
    QString notificationPreviewDisplay() const {
        return QString::fromStdString(config_.preview_display_notification);
    }
    QString defaultNotificationPreviewDisplay() const { return QString(); }
    void setNotificationPreviewDisplay(const QString& value);

    // VOCEM_CONFIG_SCREEN=1920x1080, or nothing at all. Read once: it is a property
    // of the run, not of the session.
    static qreal pinnedScreenAspect() {
        const char* pinned = std::getenv("VOCEM_CONFIG_SCREEN");
        if (!pinned || !*pinned) {
            return 0.0;
        }
        const QStringList parts = QString::fromLocal8Bit(pinned).split('x');
        if (parts.size() != 2) {
            return 0.0;
        }
        const int width = parts[0].toInt();
        const int height = parts[1].toInt();
        return height > 0 && width > 0 ? static_cast<qreal>(width) / height : 0.0;
    }
    QString version() const { return QStringLiteral(VOCEM_VERSION); }
    bool vulkanLayerInstalled() const;
    bool openglPreloadActive() const;
    // The first of these names the session's icon theme actually has, so a window
    // on Adwaita is not left with holes where Breeze's names were. The last is
    // returned unconditionally, so the caller still gets a name to fall over on.
    Q_INVOKABLE QString icon(const QStringList& names) const;
    // The size is passed in rather than set in QML, because a grouped property
    // cannot be both assigned a font and have its parts overwritten.
    Q_INVOKABLE QFont overlayFont(qreal points, bool strong) const;
    qreal overlayFontRatio() const;

    QVariantList liveInstances() const { return live_instances_; }
    QVariantList crashReports() const { return crash_reports_; }
    QVariantList journalHistory() const { return journal_history_; }
    int abiVersion() const { return static_cast<int>(vocem::kAbiVersion); }
    int segmentAbiVersion() const { return static_cast<int>(vocem::peek_abi_version()); }
    QString daemonLog() const { return daemon_log_; }
    // A journal's whole text, read on demand: the history list would be heavy
    // carrying every session's text it may never show.
    Q_INVOKABLE QString journalText(const QString& path) const;
    Q_INVOKABLE void crashCopy(const QString& path) const;
    // Acknowledges one report: the journal is deleted, the list moves on.
    Q_INVOKABLE void crashDismiss(const QString& path);
    // Empties the Sessions list: every journal it lists, crashed and clean
    // alike, through the same guard one Dismiss goes through. Exactly what the
    // page shows and nothing else -- a process that is still drawing keeps its
    // running journal, because that file is the record of a session nobody has
    // finished yet, and the daemon's journald lines are not ours to delete.
    Q_INVOKABLE void clearJournals();
    Q_INVOKABLE void refreshDaemonLog();

signals:
    void configChanged();
    void pendingChanged();
    void applicationsChanged();
    void stateChanged();
    void crashReportsChanged();
    void journalChanged();
    void liveInstancesChanged();
    void selfVoiceChanged();

private:
    void persist();
    void persistNow();
    void refreshState();
    void refreshApplications();
    void refreshCrashReports();
    void refreshLiveInstances();
    bool startDaemon();
    void stopDaemon();
    QString daemonExecutable() const;

    // What the last sweep of the registry found, and the countdown to the next
    // one. Reading a directory of small files twice a second, for a page that is
    // usually not open, would be a waste; a few seconds late is not.
    QVariantList applications_;
    int application_ticks_ = 0;
    // What the Debug section shows: every crash journal with its text, the
    // finished sessions, and the daemon's log lines when asked for.
    QVariantList crash_reports_;
    QVariantList journal_history_;
    QVariantList live_instances_;
    QString daemon_log_;
    // The journalctl ask in flight, if any: this runs asynchronously, because
    // waiting for it froze the window for as long as journald took.
    QProcess* daemon_log_process_ = nullptr;
    // The desktop entries on the machine, read once and re-read when something
    // turns up that none of them accounts for -- an application installed while
    // this window was open. Rebuilding it walks every applications directory, so
    // it is not something to do on a timer.
    vocem::DesktopEntries desktop_entries_;
    bool entries_loaded_ = false;
    bool rescanned_ = false;

    // What the window shows and edits (config_), and what is in the file
    // (saved_). They differ while an edit is waiting for Apply. This sentence
    // sat twenty lines up, on applications_, whose own comment follows it --
    // a comment off its subject reads as a comment about the wrong one.
    vocem::Config config_;
    vocem::Config saved_;
    bool pending_ = false;
    vocem::StateReader reader_;
    vocem::Snapshot snapshot_;
    SelfVoice self_voice_ = NotInChannel;
    bool attached_ = false;
    bool busy_ = false;
    QTimer timer_;
};

#endif  // VOCEM_CONFIG_BRIDGE_H
