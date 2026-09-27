// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "config_bridge.h"

#include "environment.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QPointer>
#include <QStandardPaths>
#include <QVariantMap>
#include <QWindow>

#include <cstdio>
#include <memory>
#include <utility>

// The window is the scanner's side of the session journal; the injected code
// and the daemon are the writers'. Both halves live in the one header
// (vocem/journal.h).
#define VOCEM_JOURNAL_SCANNER
#include "vocem/journal.h"

#include <algorithm>

#include "vocem/paths.h"
#include "vocem/theme.h"

QStringList ConfigBridge::fontFamilies() const { return vocem::installed_font_families(); }

QString ConfigBridge::builtInFontFamily() const { return vocem::overlay_fonts().body; }

QFont ConfigBridge::overlayFont(qreal points, bool strong) const {
    const vocem::OverlayFonts& fonts = vocem::overlay_fonts();
    QFont font;
    // The chosen family first, then Inter, then the punctuation and the emoji:
    // the same order the atlas merges them in, so the preview falls through to
    // the same face the game does for a glyph the chosen font does not carry.
    const QString chosen = QString::fromStdString(config_.font_family);
    QStringList families;
    if (!chosen.isEmpty()) {
        families.append(chosen);
    }
    families.append(strong ? fonts.strong : fonts.body);
    families.append({fonts.punctuation, fonts.emoji});
    font.setFamilies(families);
    font.setPointSizeF(points);
    font.setHintingPreference(QFont::PreferNoHinting);
    if (strong) {
        font.setWeight(QFont::DemiBold);
    }
    return font;
}

// Not a constant any more, and it must not be: the ratio between ImGui's font
// size and Qt's is a property of the file, so a preview drawn in a chosen family
// and laid out against Inter's proportion would be the wrong width by however
// much the two faces differ.
qreal ConfigBridge::overlayFontRatio() const {
    return vocem::overlay_font_ratio_for(QString::fromStdString(config_.font_family));
}
QString ConfigBridge::screenResolution() const { return vocem::screen_resolution(); }

// The connected displays, for the dropdown beside each map. From the one
// enumeration in environment.h; the value is cached there, so a binding
// re-evaluated costs a list copy and no /sys walk -- and the binding is only
// re-evaluated when displaysChanged says the enumeration moved.
QVariantList ConfigBridge::displays() const {
    QVariantList list;
    for (const vocem::DisplayMode& display : vocem::displays()) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = display.name;
        entry[QStringLiteral("width")] = display.width;
        entry[QStringLiteral("height")] = display.height;
        list.append(entry);
    }
    return list;
}

int ConfigBridge::overlayDisplayHeight() const {
    return static_cast<int>(vocem::overlay_display_height());
}

qreal ConfigBridge::sizingDisplayAspect() const { return vocem::sizing_display_aspect(); }

void ConfigBridge::setPanelPreviewDisplay(const QString& value) {
    const std::string next = value.toStdString();
    if (config_.preview_display_panel == next) {
        return;
    }
    config_.preview_display_panel = next;
    persist();
}

void ConfigBridge::setNotificationPreviewDisplay(const QString& value) {
    const std::string next = value.toStdString();
    if (config_.preview_display_notification == next) {
        return;
    }
    config_.preview_display_notification = next;
    persist();
}
bool ConfigBridge::vulkanLayerInstalled() const { return vocem::vulkan_layer_installed(); }

QVariantMap ConfigBridge::counters() const {
    QVariantMap map;
    map[QStringLiteral("stateChangedEmits")] = static_cast<qlonglong>(state_emits_);
    map[QStringLiteral("abiPeeks")] = static_cast<qlonglong>(abi_peeks_);
    map[QStringLiteral("displayReads")] = static_cast<qlonglong>(display_reads_);
    map[QStringLiteral("procWalks")] = static_cast<qlonglong>(proc_walks_);
    map[QStringLiteral("applicationSweeps")] = static_cast<qlonglong>(application_sweeps_);
    map[QStringLiteral("entryScans")] = static_cast<qlonglong>(desktop_entries_.refreshes());
    map[QStringLiteral("iconLookups")] = static_cast<qlonglong>(desktop_entries_.lookups());
    map[QStringLiteral("entriesExamined")] = static_cast<qlonglong>(desktop_entries_.entriesExamined());
    map[QStringLiteral("desktopEntries")] = desktop_entries_.size();
    return map;
}

// Whether the preload is in this session, and if not, whether the user
// manager carries it -- asked of systemctl without waiting for it. The instant
// half -- this process's own environment -- is answered in the constructor; a
// hit there is the whole answer and nothing is spawned. Otherwise the spawn
// runs beside the window's first frame, and the Debug page says it is asking
// until the answer is in. Capped at three seconds, as the synchronous version
// was, and "no answer" reads as "not in the manager" -- but off the first
// frame's path.
//
// The manager's answer is its own fact and never "active": a program launched
// from the desktop inherits the desktop's environment, fixed at login, not the
// manager's -- so with the environment.d file installed after login the
// manager has the preload and no game started from Plasma gets it until the
// next login. This used to set openglPreloadActive from either half, and the
// page said "Active in this session" and "Ready" for exactly that case.
void ConfigBridge::probePreload() {
    opengl_preload_active_ = vocem::opengl_preload_in_own_environment();
    if (opengl_preload_active_) {
        opengl_preload_known_ = true;
        return;
    }
    // A QPointer, not a raw one: the cap fires three seconds later whatever
    // happened, and the process may have finished and been deleted by then --
    // the first version dereferenced it from the timer and died there
    // (SIGSEGV in QProcess::state, measured on the second run of the window).
    QPointer<QProcess> probe = new QProcess(this);
    const auto settle = [this, probe](bool in_manager) {
        if (opengl_preload_known_) {
            return;  // answered already: by the cap, or by the process
        }
        opengl_preload_in_manager_ = in_manager;
        opengl_preload_known_ = true;
        if (probe) {
            probe->deleteLater();
        }
        emit environmentChanged();
    };
    connect(probe, &QProcess::finished, this, [probe, settle](int code, QProcess::ExitStatus status) {
        settle(status == QProcess::NormalExit && code == 0 && probe &&
               vocem::opengl_preload_in_manager_output(probe->readAllStandardOutput()));
    });
    connect(probe, &QProcess::errorOccurred, this, [settle] { settle(false); });
    QTimer::singleShot(3000, this, [probe, settle] {
        if (probe && probe->state() != QProcess::NotRunning) {
            probe->kill();
        }
        settle(false);
    });
    probe->start(QStringLiteral("systemctl"), vocem::opengl_preload_probe_arguments());
}

// A packaged install has a systemd user unit; a build tree does not. Preferring
// systemctl keeps one owner of the process, so the daemon started from here is the
// same one the session starts at login. Asked once, of `systemctl --user cat`,
// and asynchronously: it used to be a synchronous spawn with a three-second cap
// in the constructor, before the first frame. A start or a stop asked for before
// the answer is in waits for it (startDaemon, stopDaemon).
//
// No answer inside the cap is NOT "no unit": it is a busy login or a manager
// still coming up, which is exactly when the unit is about to start vocemd
// itself -- and reading it as "no unit" made startDaemon exec a second one
// beside it. The cap falls back on the unit files systemd would read
// (daemon_unit_on_disk). A systemctl that cannot be run at all is a machine
// without systemd, and there the answer is no.
void ConfigBridge::probeUnit() {
    QPointer<QProcess> probe = new QProcess(this);  // a QPointer: see probePreload
    const auto settle = [this, probe](bool available) {
        if (unit_available_ >= 0) {
            return;
        }
        unit_available_ = available ? 1 : 0;
        if (probe) {
            probe->deleteLater();
        }
        // A stop asked for while the question was open wins over a start asked
        // for before it: the window opened and was closed again, and Quit means
        // everything down.
        if (daemon_stop_wanted_) {
            daemon_start_wanted_ = false;
            stopDaemon(std::exchange(daemon_stop_wanted_, nullptr));
        } else if (daemon_start_wanted_) {
            daemon_start_wanted_ = false;
            startDaemon();
        }
    };
    connect(probe, &QProcess::finished, this, [settle](int code, QProcess::ExitStatus status) {
        settle(status == QProcess::NormalExit && code == 0);
    });
    connect(probe, &QProcess::errorOccurred, this, [settle](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            settle(false);
        }
    });
    QTimer::singleShot(3000, this, [probe, settle] {
        if (probe && probe->state() != QProcess::NotRunning) {
            probe->kill();
        }
        settle(vocem::daemon_unit_on_disk());
    });
    probe->start(QStringLiteral("systemctl"),
                 {QStringLiteral("--user"), QStringLiteral("cat"), QStringLiteral("vocemd.service")});
}

QString ConfigBridge::icon(const QStringList& names) const { return vocem::theme_icon(names); }

ConfigBridge::ConfigBridge(QObject* parent) : QObject(parent) {
    // Registered before any preview asks for it.
    vocem::overlay_fonts();
    config_.load();
    disk_mtime_ = vocem::Config::mtime();
    // What is on disk wins over what the file remembers: somebody may have removed
    // the entry through their desktop's own startup-applications window, and the
    // switch has to say what is true rather than what was asked for.
    config_.start_at_login = vocem::autostart_enabled();
    saved_ = config_;

    // Half a second is fast enough to feel live and slow enough to be free.
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &ConfigBridge::refreshState);
    timer_.start();
    // The two spawns, started and not waited for. The harness runs ask only
    // the preload question, which reads the manager's environment and changes
    // nothing (tests/window_startup.cmake measures the first frame against a
    // slow one); the unit question is theirs to skip, because nothing under
    // the harness starts or stops the daemon -- see startDaemon/stopDaemon.
    harness_ = qEnvironmentVariableIsSet("VOCEM_CONFIG_GEOMETRY") ||
               qEnvironmentVariableIsSet("VOCEM_CONFIG_SCREENSHOT") ||
               qEnvironmentVariableIsSet("VOCEM_CONFIG_NO_DAEMON");
    probePreload();
    if (!harness_) {
        probeUnit();
    }
    refreshDisplays();
    refreshState();
    refreshApplications();
    // The live card has to be right the moment the page opens, not a tick later:
    // somebody who alt-tabs out of a game to look is asking about *now*.
    refreshLiveInstances();

    // The window and the overlay rise and fall together now: opened means the
    // overlay is up, Quit means everything down until the next opening. This is
    // the rising half -- if the daemon is not there, start it, whether the
    // window was opened from the menu or arrived hidden with the session's
    // autostart. The falling half is quitOverlay().
    //
    // Not during a harness run: the offscreen geometry dump and the screenshot
    // walk drive this window headless, and a measurement must not reach into
    // the session's services. VOCEM_CONFIG_NO_DAEMON says the same for a run
    // that is not a dump -- tests/single_instance.cmake, which needs the
    // window's own startup path and none of its services.
    if (!attached_ && !harness_) {
        startDaemon();
    }
}

// A setting is edited here and written on Apply, not on the way past.
//
// Everything used to save on the keystroke, which is defensible for a window whose
// every control has a picture beside it -- and wrong for the same reason: dragging
// a slider from one end to the other wrote the file forty times and moved the
// overlay in a running game forty times. The window now edits its own copy, the
// previews follow it immediately, and the game sees one write when Apply is
// pressed.
void ConfigBridge::persist() {
    if (!pending_) {
        pending_ = true;
        emit pendingChanged();
    }
    emit configChanged();
}

// The exceptions, and the reason they are exceptions: these are not settings on a
// page with an Apply button under it. They are the switches that decide whether
// anything is drawn at all, reachable from the header and from the tray, and a
// switch that does nothing until a button on some other page is pressed is a
// broken switch. Written straight through, on top of the file as it stands, so a
// half-finished edit on another page is neither applied nor lost.
void ConfigBridge::persistNow(bool vocem::Config::*which) {
    // On top of the file as it is NOW, not as it was when this window started,
    // and only the switch that moved: Config::write_switch loads the file
    // fresh, sets that one key, and saves.
    vocem::Config written;
    const bool saved = vocem::Config::write_switch(which, config_.*which, &written);
    if (reportSave(saved)) {
        const vocem::Config followed = saved_;
        saved_ = written;
        saved_.start_at_login = config_.start_at_login;
        // This write IS the file moving under the window, and `written` is the
        // file as it now stands -- every key, including anything edited outside
        // since this window opened, which write_switch reloads rather than
        // overwrites (entry 136). The copy the window shows and Apply writes
        // takes it from here, when nothing is waiting for Apply: without the
        // line below the window went on showing its own morning copy, the Apply
        // button stayed grey because nothing was pending, nothing said the two
        // differed, and the next Apply wrote the window's copy over the edit.
        // The timestamp is not advanced: the next sweep reloads the file it
        // just wrote, which changes nothing, and a write that lands between
        // this save and that sweep is read rather than adopted unread.
        //
        // With an edit waiting for Apply the window keeps its own edits, and
        // only those, exactly as reloadIfMoved decides it: every key it did not
        // change follows the file (Config::merged), so neither the window nor
        // the next Apply carries a value the file no longer has.
        // start_at_login is read from the autostart entry, not from the file
        // (which carries the key and is not believed) -- so it is carried
        // across rather than taken from the read.
        const bool login = config_.start_at_login;
        config_ = pending_ ? vocem::Config::merged(followed, config_, written) : written;
        config_.start_at_login = login;
    } else {
        // The write failed and the switch in the window has already moved. Left
        // alone, the window shows "on" against a file that says "off" for the
        // rest of the session, with a grey Apply and nothing to press: entry
        // 136 made apply() leave the edit pending for this exact reason and
        // persistNow() was outside that fix. The InlineMessage says why; this
        // is what makes the edit recoverable.
        if (!pending_) {
            pending_ = true;
            emit pendingChanged();
        }
    }
    emit configChanged();
}

void ConfigBridge::apply() {
    if (!pending_) {
        return;
    }
    // What the window changed, on top of the file as it stands: config_ is
    // the window's copy, saved_ the file it last followed, and a key the
    // window did not change takes the file's value now (Config::merged).
    // Saving config_ whole wrote back every key edited outside the window
    // since it last followed the file -- a hand-written flatpak_apps, which
    // the window has no control for, went back to its startup value.
    vocem::Config fresh;
    fresh.load();
    vocem::Config written = vocem::Config::merged(saved_, config_, fresh);
    // Not believed from the file (it is read from the autostart entry): the
    // window's value is the intent, carried out below whatever saved_ says.
    written.start_at_login = config_.start_at_login;
    if (!reportSave(written.save())) {
        return;  // still pending: the button stays live and the message says why
    }
    config_ = written;
    saved_ = config_;
    disk_mtime_ = vocem::Config::mtime();
    // The autostart entry is a file rather than a line in the settings, so it is
    // made to match here: the setting is the intent, the entry is the effect.
    // Only when the two differ -- see set_autostart -- and a write that fails
    // is said, and leaves the edit waiting, as a settings file that could not
    // be written does.
    if (config_.start_at_login != vocem::autostart_enabled() &&
        !vocem::set_autostart(config_.start_at_login)) {
        reportFailure(tr("The login entry could not be written to %1. Check that the directory "
                         "exists and is writable.")
                          .arg(vocem::autostart_entry_path()));
        return;
    }
    pending_ = false;
    emit pendingChanged();
    emit configChanged();
}

bool ConfigBridge::reportSave(bool saved) {
    if (!saved) {
        reportFailure(tr("The settings could not be written to %1. Check that the directory "
                         "exists and is writable.")
                          .arg(QString::fromStdString(vocem::Config::path())));
        return false;
    }
    if (!save_error_.isEmpty()) {
        save_error_.clear();
        emit saveErrorChanged();
    }
    return true;
}

void ConfigBridge::reportFailure(const QString& error) {
    if (error != save_error_) {
        save_error_ = error;
        emit saveErrorChanged();
    }
    qWarning("vocem-config: %s", qPrintable(error));
}

void ConfigBridge::reloadIfMoved() {
    const long long mtime = vocem::Config::mtime();
    if (mtime == disk_mtime_) {
        return;
    }
    disk_mtime_ = mtime;
    vocem::Config fresh;
    fresh.load();
    fresh.start_at_login = vocem::autostart_enabled();
    // An edit waiting for Apply keeps the window's edits: Apply means "what the
    // window shows", and a reload underneath it would take them away. Every
    // key the window did not change follows the file even then
    // (Config::merged): the window kept its whole copy here once, and the next
    // Apply wrote the keys edited outside back to their old values. With
    // nothing pending the window follows the file, as the game does.
    config_ = pending_ ? vocem::Config::merged(saved_, config_, fresh) : fresh;
    saved_ = fresh;
    emit configChanged();
}

void ConfigBridge::setNumber(const char* key, float vocem::Config::*member, qreal value) {
    const float clamped = static_cast<float>(vocem::Config::clamped(key, value));
    if (qFuzzyCompare(config_.*member, clamped)) {
        return;
    }
    config_.*member = clamped;
    persist();
}

namespace {

// The evidence the process wrote down, as a sentence about where the answer came
// from rather than a verdict on the application. The record carries a token so
// that it stays readable in a file and greppable in a log; the window is where it
// has to be words a person is comfortable reading about their own software --
// "Not a game" told somebody they disagreed with the window and nothing else.
//
// The point of showing it at all is the case the detection gets wrong. `not-ours`
// says the process was started from an entry that names a different program,
// which is the launcher-and-its-child case -- exactly the shape of a game that is
// still missed -- and a switch flipped without knowing that is a workaround
// rather than a fix.
QString reason_text(const std::string& reason) {
    const size_t colon = reason.find(':');
    const std::string kind = reason.substr(0, colon);
    const QString detail =
        colon == std::string::npos ? QString() : QString::fromStdString(reason.substr(colon + 1));

    if (kind == "steam") return ConfigBridge::tr("Identified by Steam (app id %1)").arg(detail);
    if (kind == "umu") return ConfigBridge::tr("Identified by umu (%1)").arg(detail);
    if (kind == "lutris") return ConfigBridge::tr("Identified by Lutris");
    if (kind == "heroic") return ConfigBridge::tr("Identified by Heroic");
    if (kind == "minecraft") return ConfigBridge::tr("Identified as Minecraft");
    if (kind == "gamescope") return ConfigBridge::tr("Identified by gamescope");
    if (kind == "itch") return ConfigBridge::tr("Identified by the itch.io app");
    if (kind == "desktop")
        return ConfigBridge::tr("Identified by its desktop entry (%1)").arg(detail);
    // Not an entry anybody pointed at: the one installed entry that runs this
    // program, found by looking. That is what a game started from a terminal or a
    // file manager has instead of nothing, so the sentence says it was found.
    if (kind == "entry")
        return ConfigBridge::tr("Found the desktop entry that runs it (%1)").arg(detail);
    if (kind == "flatpak")
        return ConfigBridge::tr("Identified by its Flatpak id (%1)").arg(detail);
    if (kind == "launcher")
        return ConfigBridge::tr("The overlay follows the games it starts instead");
    if (kind == "not-ours")
        return ConfigBridge::tr("Started from %1, which belongs to a different application")
            .arg(detail);
    if (kind == "not-a-game")
        return ConfigBridge::tr("Its desktop entry (%1) describes a regular application")
            .arg(detail);
    if (kind == "no-entry")
        return ConfigBridge::tr("No desktop entry found under the name %1").arg(detail);
    if (kind == "nothing")
        return ConfigBridge::tr("No launcher id, game arguments or desktop entry to go on");
    // A record written before the evidence was kept, and anything a later version
    // learns to write. Neither is worth a row that says "unknown".
    return {};
}

// Which of the page's groups this application belongs in. The verdict decides the
// first; the evidence's own prefix tells the others apart -- the tokens are stable
// on purpose (DESIGN, "Saying what it went on"), which is what makes them safe to
// build interface on. `nothing`, `not-ours:`, `no-entry:` and the records written
// before the evidence was kept all land in the last group: the cases where the
// detection had nothing it could believe, and where the switch is the remedy.
QString category_of(bool game, const std::string& reason) {
    if (game) {
        return QStringLiteral("game");
    }
    const std::string kind = reason.substr(0, reason.find(':'));
    if (kind == "launcher") {
        return QStringLiteral("launcher");
    }
    if (kind == "not-a-game") {
        return QStringLiteral("other");
    }
    return QStringLiteral("unrecognised");
}

}  // namespace

// The registry, as the window shows it: newest first, each one carrying whether a
// rule currently keeps the overlay out of it. Swept every few seconds rather than
// on every tick, and the signal is only emitted when the answer changed -- this
// feeds a list, and a list that is rebuilt twice a second cannot be scrolled.
void ConfigBridge::refreshApplications() {
    std::vector<vocem::Application> found = vocem::known_applications();
    std::sort(found.begin(), found.end(),
              [](const vocem::Application& a, const vocem::Application& b) {
                  return a.seen > b.seen;
              });

    if (!entries_loaded_) {
        entries_loaded_ = true;
        desktop_entries_.refresh();
    }

    // The list, and whether anything in it has no icon. Built twice at most: once
    // from the desktop entries as they were last read, and again if something in it
    // was not accounted for and the entries have not been re-read since the set of
    // applications last changed -- an application installed while this window was
    // open gets one fresh look, and one that will never have an entry (a game under
    // a path its store invented) does not cost a directory walk every few seconds.
    //
    // A loop and not a call to itself. It was written as a call to itself, and the
    // latch that was supposed to stop it was lifted by the same condition that
    // started it: the window recursed until the stack was gone, which the
    // screenshot run turned up as a segmentation fault at startup.
    // Which of the recorded names has a live process at this moment: one walk
    // of /proc per refresh (every four seconds, and only while the window is
    // up), comm per pid, into a set the loop below asks. The kernel truncates
    // comm to fifteen characters, so the record's key -- which came from the
    // same file -- is compared whole.
    QSet<QString> running_now;
    if (!found.empty()) {
        ++proc_walks_;
        const QDir proc(QStringLiteral("/proc"));
        const QStringList pids =
            proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& pid : pids) {
            if (pid.isEmpty() || pid.at(0) < QLatin1Char('0') || pid.at(0) > QLatin1Char('9')) {
                continue;
            }
            QFile comm(QStringLiteral("/proc/") + pid + QStringLiteral("/comm"));
            if (comm.open(QIODevice::ReadOnly)) {
                running_now.insert(QString::fromUtf8(comm.readAll()).trimmed());
            }
        }
    }

    ++application_sweeps_;
    QVariantList list;
    for (int attempt = 0; attempt < 2; ++attempt) {
        list.clear();
        bool unresolved = false;

        for (const vocem::Application& application : found) {
            QVariantMap entry;
            const QString name = QString::fromStdString(application.key);
            const QString executable = QString::fromStdString(application.executable);
            const QString icon = desktop_entries_.iconFor(
                executable, name, QString::fromStdString(application.desktop),
                QString::fromStdString(application.steam_app_id));
            unresolved = unresolved || icon.isEmpty();

            entry["name"] = name;
            entry["executable"] = executable;
            entry["api"] = QString::fromStdString(application.api);
            entry["seen"] = QDateTime::fromSecsSinceEpoch(application.seen);
            entry["icon"] = icon;
            // What the overlay will actually do there, worked out the same way the
            // injected code works it out: a game unless said otherwise, and the two
            // lists are the deviations.
            const std::string key = application.key;
            const std::string binary = QFileInfo(executable).fileName().toStdString();
            const bool excluded = vocem::listed(config_.hidden_apps, key) ||
                                  vocem::listed(config_.hidden_apps, binary);
            const bool asked_for = vocem::listed(config_.shown_apps, key) ||
                                   vocem::listed(config_.shown_apps, binary);
            // What the process itself concluded, which is the answer the injected
            // code will act on. A record written before the detection existed does
            // not carry it, so a Steam id in the same record stands in: it is the
            // same evidence, and it is what made that process a game in the first
            // place. Everything else sorts itself out the next time it runs.
            const bool game = application.looks_like_game || !application.steam_app_id.empty();
            entry["game"] = game;
            entry["reason"] = reason_text(application.reason);
            // The token as the record carries it, beside the sentence made of it:
            // the page groups by the sentence's kind and keeps the raw evidence
            // reachable, because a case being argued about is quoted from the
            // record, not from a translation.
            entry["evidence"] = QString::fromStdString(application.reason);
            entry["category"] = category_of(game, application.reason);
            entry["drawn"] = excluded ? false : (asked_for || game);
            // Whether a process by this name is alive right now -- what lets
            // the page put "running this moment, overlay off" above the
            // history instead of leaving it buried there.
            entry["running"] = running_now.contains(name);
            list.append(entry);
        }

        if (list.size() != applications_.size()) {
            rescanned_ = false;
        }
        if (attempt == 0 && unresolved && !rescanned_) {
            rescanned_ = true;
            desktop_entries_.refresh();
            continue;
        }
        break;
    }

    if (list != applications_) {
        applications_ = list;
        emit applicationsChanged();
    }
}

void ConfigBridge::setHiddenApps(const QString& value) {
    const std::string next = value.toStdString();
    if (config_.hidden_apps == next) {
        return;
    }
    config_.hidden_apps = next;
    persist();
    refreshApplications();
}


// The switch beside one application, which says whether the overlay draws there.
//
// There is no single list behind it. The default is the detection -- a game gets
// the overlay and nothing else does -- and the two lists are only the deviations
// from that, so a switch left where the detection put it writes nothing at all.
void ConfigBridge::setShownApps(const QString& value) {
    const std::string next = value.toStdString();
    if (config_.shown_apps == next) {
        return;
    }
    config_.shown_apps = next;
    persist();
    refreshApplications();
}

void ConfigBridge::setApplicationDrawn(const QString& name, bool drawn, bool game) {
    const std::string key = name.toStdString();
    if (key.empty()) {
        return;
    }
    // A name the lists cannot hold -- a comma, or a space at either end -- is
    // refused and said, and the checkbox is put back by re-announcing the
    // list it reads: stored, "Foo, Bar" hid the applications Foo and Bar.
    if (!vocem::list_entry_fits(key)) {
        reportFailure(tr("\"%1\" cannot be put in the list of applications: a name with a comma "
                         "in it, or a space at either end, would be read back as another name.")
                          .arg(name));
        emit applicationsChanged();
        return;
    }

    // Both spellings a rule may use are taken out: the process name, and the
    // executable's own name, which config.h says a rule may be written
    // against and which the list page honours when it reads. The switch used
    // to remove the process name alone, so a hand-written rule on the
    // executable's name snapped the box back the moment it was ticked.
    std::string binary;
    for (const QVariant& entry : applications_) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("name")).toString() == name) {
            binary = QFileInfo(map.value(QStringLiteral("executable")).toString())
                         .fileName()
                         .toStdString();
            break;
        }
    }
    for (const std::string& spelling : {key, binary}) {
        if (spelling.empty()) {
            continue;
        }
        config_.hidden_apps = vocem::list_without(config_.hidden_apps, spelling);
        config_.shown_apps = vocem::list_without(config_.shown_apps, spelling);
    }
    if (game && !drawn) {
        config_.hidden_apps = vocem::list_with(config_.hidden_apps, key);
    } else if (!game && drawn) {
        config_.shown_apps = vocem::list_with(config_.shown_apps, key);
    }

    persist();
    refreshApplications();
}

void ConfigBridge::forgetApplications() {
    vocem::forget_applications();
    refreshApplications();
}

void ConfigBridge::refreshLiveInstances() {
    // The journals whose process is still there: one entry per drawing process,
    // newest pid first so a game started later is nearer the top. The name is the
    // kernel's, truncated to fifteen characters like everything else that comes out
    // of /proc/comm, which is why the page matches it against the records rather
    // than trying to prettify it here. The daemon keeps a journal too and is not
    // an instance of the overlay drawing, so its row is filtered here and shown
    // by the Debug page's own daemon card instead.
    QVariantList fresh;
    for (const vocem::JournalEntry& live : vocem::journal_live()) {
        if (live.api == "daemon") {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("name")] = QString::fromStdString(live.process);
        entry[QStringLiteral("pid")] = live.pid;
        entry[QStringLiteral("api")] = QString::fromStdString(live.api);
        entry[QStringLiteral("path")] = QString::fromStdString(live.path);
        // The counters the process keeps beside its journal: how many presents
        // it was willing to draw in, and how many frames it actually painted.
        // Zero-zero for a library from before the counters existed.
        long frames = 0;
        long drawn = 0;
        if (vocem::journal_read_stat_beside(live.path, frames, drawn)) {
            entry[QStringLiteral("frames")] = static_cast<qlonglong>(frames);
            entry[QStringLiteral("drawn")] = static_cast<qlonglong>(drawn);
        }
        fresh.append(entry);
    }
    std::sort(fresh.begin(), fresh.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("pid")).toInt() >
               b.toMap().value(QStringLiteral("pid")).toInt();
    });
    if (fresh != live_instances_) {
        live_instances_ = fresh;
        emit liveInstancesChanged();
    }
}

// Every report at once, not one at a time: the pop-up held one story because a
// window holds one story, and the Debug section is a list.
//
// The text is NOT in the model. It was, on the reasoning that a crash report
// exists to be read and there are never more than a handful -- and measured,
// that reasoning cost 780 MB of resident memory and four and a half seconds of
// startup for one 10 MB journal, paid whether or not anybody opened the page,
// because every page of the window is built at launch. The list carries the
// header only; the page asks for a text when it shows one.
void ConfigBridge::refreshCrashReports() {
    QVariantList fresh;
    for (const vocem::JournalEntry& report : vocem::journal_crashes()) {
        QVariantMap entry;
        entry[QStringLiteral("process")] = QString::fromStdString(report.process);
        entry[QStringLiteral("pid")] = report.pid;
        entry[QStringLiteral("api")] = QString::fromStdString(report.api);
        entry[QStringLiteral("path")] = QString::fromStdString(report.path);
        entry[QStringLiteral("when")] =
            QDateTime::fromSecsSinceEpoch(report.when).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        fresh.append(entry);
    }
    if (fresh != crash_reports_) {
        crash_reports_ = fresh;
        emit crashReportsChanged();
    }

    QVariantList history;
    for (const vocem::JournalEntry& done : vocem::journal_history()) {
        QVariantMap entry;
        entry[QStringLiteral("process")] = QString::fromStdString(done.process);
        entry[QStringLiteral("pid")] = done.pid;
        entry[QStringLiteral("api")] = QString::fromStdString(done.api);
        entry[QStringLiteral("path")] = QString::fromStdString(done.path);
        entry[QStringLiteral("when")] =
            QDateTime::fromSecsSinceEpoch(done.when).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        history.append(entry);
    }
    if (history != journal_history_) {
        journal_history_ = history;
        emit journalChanged();
    }
}

namespace {

// A journal file is a file in the journal's own directory -- resolved, not
// spelled. The first version compared `QFileInfo::absolutePath()` as a string,
// which cleans `..` but does NOT resolve symlinks: a symlink planted inside
// the journal directory passed the check and was followed, so the Debug
// section displayed -- and the Copy button copied -- any file the user could
// read. Worse than an invokable somebody has to call: the four-second scan
// picked such a link up by itself, unattended. Measured with a link to
// /etc/passwd, which duly appeared in the window. `canonicalFilePath()`
// resolves the whole chain and returns empty for what does not exist, and the
// directory is canonicalised too so that a spelling with a doubled slash --
// which made Dismiss silently do nothing, forever -- cannot miss either.
bool inside_journal_directory(const QString& path) {
    const QString resolved = QFileInfo(path).canonicalFilePath();
    if (resolved.isEmpty()) {
        return false;
    }
    const QString dir = QFileInfo(resolved).absolutePath();
    for (const std::string& root : {vocem::journal_dir(), vocem::journal_legacy_dir()}) {
        const QString canonical = QFileInfo(QString::fromStdString(root)).canonicalFilePath();
        if (!canonical.isEmpty() && dir == canonical) {
            return true;
        }
    }
    return false;
}

// What one journal may contribute to this window's memory. A journal is a
// handful of lines by construction; anything past this is not a journal, and
// a settings window is not the place to find out how large it is (measured:
// one 10 MB file cost 780 MB resident and 4.5 s of startup).
constexpr qint64 kJournalTextCap = 128 * 1024;

}  // namespace

QString ConfigBridge::journalText(const QString& path) const {
    if (!inside_journal_directory(path)) {
        return QString();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QString text = QString::fromUtf8(file.read(kJournalTextCap));
    if (file.bytesAvailable() > 0) {
        text += tr("\n… truncated: this journal is larger than a journal should be.\n");
    }
    return text;
}

void ConfigBridge::crashCopy(const QString& path) const {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(journalText(path));
    }
}

void ConfigBridge::crashDismiss(const QString& path) {
    if (inside_journal_directory(path)) {
        QFile::remove(path);
        // The counters go with the journal. Only the clean-exit path used to
        // remove them, so a crashed process left its .stat behind for good --
        // small, but it accumulated in the directory both walks scan.
        QString stat = path;
        if (stat.endsWith(QStringLiteral(".running"))) {
            stat.chop(8);
            QFile::remove(stat + QStringLiteral(".stat"));
        }
    }
    refreshCrashReports();
    refreshLiveInstances();
}

// The whole Sessions list at once. It walks the two lists the page draws --
// which is what makes "Clear" mean what the page shows rather than "empty the
// directory": a live process's journal is in neither, and removing it under a
// game that is still drawing would take away the one file that says what the
// overlay was doing if that game then dies. Each path still goes through
// inside_journal_directory(), so a planted symlink is refused here exactly as
// it is refused everywhere else in this file.
void ConfigBridge::clearJournals() {
    const QVariantList lists[] = {crash_reports_, journal_history_};
    for (const QVariantList& list : lists) {
        for (const QVariant& entry : list) {
            const QString path = entry.toMap().value(QStringLiteral("path")).toString();
            if (path.isEmpty() || !inside_journal_directory(path)) {
                continue;
            }
            QFile::remove(path);
            // The counters travel with their journal, as they do in
            // crashDismiss(): both suffixes, because this list holds both kinds.
            QString stat = path;
            for (const QString& suffix :
                 {QStringLiteral(".running"), QStringLiteral(".done")}) {
                if (stat.endsWith(suffix)) {
                    stat.chop(suffix.size());
                    QFile::remove(stat + QStringLiteral(".stat"));
                    break;
                }
            }
        }
    }
    refreshCrashReports();
    refreshLiveInstances();
}

// The daemon's own account, from journald: the unit's recent lines, on demand.
// Where there is no unit -- a build tree -- the daemon's journal file in the
// history above is what there is, and this says so instead of pretending.
//
// Asynchronous, because this is called from the page becoming visible and from
// a button, both on the GUI thread: the first version waited up to three
// seconds for journalctl and froze the whole window while it did (measured at
// 3003 ms against a slow stand-in). The answer arrives when it arrives.
void ConfigBridge::refreshDaemonLog() {
    if (daemon_log_process_) {
        return;  // one ask at a time; the answer refreshes the page either way
    }
    auto* journalctl = new QProcess(this);
    daemon_log_process_ = journalctl;
    connect(journalctl, &QProcess::finished, this,
            [this, journalctl](int code, QProcess::ExitStatus exit_status) {
                QString fresh;
                if (exit_status == QProcess::NormalExit && code == 0) {
                    fresh = QString::fromUtf8(journalctl->readAllStandardOutput());
                }
                if (fresh.trimmed().isEmpty()) {
                    fresh = tr("No journald lines for vocemd here. A build tree runs the "
                               "daemon outside the unit. Its session journal above is the "
                               "record.");
                }
                daemon_log_process_ = nullptr;
                journalctl->deleteLater();
                if (fresh != daemon_log_) {
                    daemon_log_ = fresh;
                    emit journalChanged();
                }
            });
    connect(journalctl, &QProcess::errorOccurred, this, [this, journalctl] {
        // Never started -- no journalctl on this machine -- which is the same
        // answer as no lines, said the same way.
        if (daemon_log_process_ != journalctl) {
            return;
        }
        daemon_log_process_ = nullptr;
        journalctl->deleteLater();
        const QString fresh = tr("No journald lines for vocemd here. A build tree runs the "
                                 "daemon outside the unit. Its session journal above is the "
                                 "record.");
        if (fresh != daemon_log_) {
            daemon_log_ = fresh;
            emit journalChanged();
        }
    });
    journalctl->start(QStringLiteral("journalctl"),
                      {QStringLiteral("--user"), QStringLiteral("-u"), QStringLiteral("vocemd"),
                       QStringLiteral("-n"), QStringLiteral("80"), QStringLiteral("--no-pager"),
                       QStringLiteral("-o"), QStringLiteral("short")});
}

// The machine's displays, re-read and announced only when they changed. They
// were read once, on first use, and kept for the life of the process -- which
// for a tray application started at login is the whole session (entry 111);
// then forgotten on every sweep and re-read by every binding on stateChanged,
// which fires twice a second. Read here, once per sweep, compared, announced
// on a signal of their own.
void ConfigBridge::refreshDisplays() {
    vocem::forget_displays();
    ++display_reads_;
    const QList<vocem::DisplayMode> fresh = vocem::displays();
    const QString resolution = vocem::screen_resolution();
    if (fresh == displays_shown_ && resolution == resolution_shown_) {
        return;
    }
    displays_shown_ = fresh;
    resolution_shown_ = resolution;
    emit displaysChanged();
}

void ConfigBridge::announceState() {
    Announced now;
    now.busy = busy_;
    now.attached = attached_;
    now.status = attached_ ? static_cast<int>(snapshot_.status) : -1;
    now.in_channel = attached_ && snapshot_.in_channel;
    now.user_count = attached_ ? snapshot_.user_count : 0;
    now.segment_abi = segment_abi_;
    if (announced_once_ && now == announced_) {
        return;
    }
    announced_ = now;
    announced_once_ = true;
    ++state_emits_;
    emit stateChanged();
}

void ConfigBridge::refreshState() {
    // Every eighth tick: four seconds, which is sooner than anybody can start a
    // game and reach the page it appears on, and rare enough that a list nobody is
    // looking at costs nothing.
    if (--application_ticks_ <= 0) {
        application_ticks_ = 8;
        reloadIfMoved();
        refreshApplications();
        refreshCrashReports();
        // The live list on the same tick as the rest. It used to be computed
        // in the constructor and never again, so a window left open before a
        // game started said "the overlay is not drawing anywhere" while the
        // overlay was drawing -- reported by the owner with Minecraft up,
        // 28223 frames in its journal, and an empty Debug page. Exactly the
        // silence this page exists to break, produced by the page itself.
        refreshLiveInstances();
        // And the machine's displays, on the same slow tick. They were read
        // once, on first use, and kept for the life of the process -- which for
        // a tray application started at login is the whole session. The daemon
        // re-reads /sys/class/drm every sixty seconds and running games follow
        // it, so a monitor plugged in after the window opened resized the
        // overlay while the map of it went on drawing the display that was
        // there at login. Forgotten here rather than re-read here: the reads
        // are what fill them again, and every one of them is a binding on
        // stateChanged, which this tick ends by emitting.
        //
        // On this tick and not the twice-a-second one, measured: 141 us per
        // enumeration on this machine, so four seconds is thirty-five parts in
        // a million and still fifteen times more often than the daemon asks.
        refreshDisplays();
        // The segment's ABI, for the Debug page's two-sided question (entry
        // 60): asked here, once per sweep, rather than by every binding on
        // every tick. Attached means the reader accepted it, which is our own.
        ++abi_peeks_;
        segment_abi_ = attached_ ? static_cast<int>(vocem::kAbiVersion)
                                 : static_cast<int>(vocem::peek_abi_version());
    }

    if (!attached_) {
        attached_ = reader_.open();
        if (!attached_) {
            announceState();
            return;
        }
        segment_abi_ = static_cast<int>(vocem::kAbiVersion);
    }
    // still_current() as well as read(): unlinking removes the name and not the
    // pages, so after an external stop this mapping would go on reading its
    // private copy of history -- a "Connected" that nobody is publishing -- and
    // after a restart it would never see the new daemon's segment. Asked here on
    // every tick, which is the same cadence the reopen retry above runs at,
    // exactly as the API's own note asks.
    if (!reader_.read(snapshot_) || !reader_.still_current()) {
        // The daemon exited and unlinked the segment, or was replaced under the
        // mapping: drop it so the next tick can pick up the living one.
        reader_.close();
        attached_ = false;
        snapshot_ = vocem::Snapshot{};
    }

    // The user's own row of the channel, reduced to the one state the tray
    // icon draws. Emitted only on change: SPEAKING events flip it a few times
    // per sentence, the tick runs twice a second, and the tray must not be
    // told the same icon name five hundred times a speech.
    SelfVoice fresh_voice = NotInChannel;
    if (attached_ && snapshot_.in_channel) {
        // In a channel, whoever we turn out to be: the quiet ring rather than
        // the not-in-a-channel portrait. Without this, a segment that carries
        // no self flag -- an older daemon, or one whose AUTHENTICATE reply
        // never named the user -- showed the tray icon of somebody not in a
        // call while the tooltip beside it said "Connected, 3 participants".
        // A fallback that is right for its own case hiding the case it was
        // not written for is entry 34's shape.
        fresh_voice = InChannelIdle;
        for (uint32_t i = 0; i < snapshot_.user_count; ++i) {
            const vocem::User& user = snapshot_.users[i];
            if (!(user.flags & vocem::kFlagSelf)) {
                continue;
            }
            if (user.flags & vocem::kFlagDeafened) {
                fresh_voice = Deafened;
            } else if (user.flags & vocem::kFlagMuted) {
                fresh_voice = Muted;
            } else if (user.flags & vocem::kFlagSpeaking) {
                fresh_voice = Speaking;
            } else {
                fresh_voice = InChannelIdle;
            }
            break;
        }
    }
    if (fresh_voice != self_voice_) {
        self_voice_ = fresh_voice;
        emit selfVoiceChanged();
    }

    announceState();
}

// The one place that decides what is going on. Everything else -- the sentence,
// the hint, the button, the colour of the dot -- is derived from it, so they can
// never disagree, and none of them has to be parsed back out of a translation.
ConfigBridge::State ConfigBridge::state() const {
    if (busy_) {
        return Working;
    }
    if (!attached_) {
        return NotRunning;
    }
    switch (snapshot_.status) {
        case vocem::DaemonStatus::WaitingForDiscord:
        case vocem::DaemonStatus::Authorising:
            return Waiting;
        case vocem::DaemonStatus::AuthorisationRefused:
            return Refused;
        case vocem::DaemonStatus::Connected:
            break;
    }
    return Connected;
}

QString ConfigBridge::statusText() const {
    if (!attached_) {
        return tr("Not running");
    }
    switch (snapshot_.status) {
        case vocem::DaemonStatus::WaitingForDiscord:
            return tr("Waiting for Discord");
        case vocem::DaemonStatus::Authorising:
            return tr("Waiting for you to accept in Discord");
        case vocem::DaemonStatus::AuthorisationRefused:
            return tr("Authorisation refused");
        case vocem::DaemonStatus::Connected:
            break;
    }
    if (!snapshot_.in_channel) {
        return tr("Connected, not in a voice channel");
    }
    // Two sentences rather than "%1 participant(s)": with no translation loaded
    // Qt shows the source text as written, so the bracketed suffix reached the
    // screen for somebody to expand in their head. (%n would need a translator
    // to pick the form; two plain strings are right today and translatable
    // tomorrow.)
    if (snapshot_.user_count == 1) {
        return tr("Connected, one participant");
    }
    return tr("Connected, %1 participants").arg(snapshot_.user_count);
}

// The hint carries the thing the user actually has to do, which the status alone
// does not always make obvious.
QString ConfigBridge::hintText() const {
    if (!attached_) {
        return tr("Start it to connect to Discord.");
    }
    switch (snapshot_.status) {
        case vocem::DaemonStatus::WaitingForDiscord:
            return tr("Open the Discord desktop application.");
        case vocem::DaemonStatus::Authorising:
            return tr("Accept the permission request in the Discord window.");
        case vocem::DaemonStatus::AuthorisationRefused:
            return tr("Permission declined. Nothing is shown until it is granted.");
        case vocem::DaemonStatus::Connected:
            return snapshot_.in_channel
                       ? tr("Ready. The overlay appears when a game starts.")
                       : tr("Join a voice channel to see participants.");
    }
    return {};
}

QString ConfigBridge::actionText() const {
    if (busy_) {
        return tr("Working…");
    }
    if (!attached_) {
        return tr("Start");
    }
    if (snapshot_.status == vocem::DaemonStatus::AuthorisationRefused) {
        return tr("Connect to Discord");
    }
    return {};
}

bool ConfigBridge::actionAvailable() const { return !busy_ && !actionText().isEmpty(); }

QString ConfigBridge::daemonExecutable() const {
    // Installed first, then next to this binary, which is what a build tree looks
    // like: build/gui/vocem-config alongside build/daemon/vocemd.
    const QString installed = QStandardPaths::findExecutable(QStringLiteral("vocemd"));
    if (!installed.isEmpty()) {
        return installed;
    }
    const QDir here(QCoreApplication::applicationDirPath());
    const QString sibling = here.absoluteFilePath(QStringLiteral("../daemon/vocemd"));
    return QFileInfo::exists(sibling) ? QFileInfo(sibling).absoluteFilePath() : QString();
}


// The harness drives this window to measure it, and a measurement must not
// reach the session's services: the Quit at the end of a run, a close with no
// tray behind it (every offscreen run has none) and Re-authorise all used to
// end in a stop, and with the unit question never asked the stop was
// `pkill -TERM -x vocemd` -- the owner's live daemon included. Said once, so a
// harness run that expected a daemon can find out why there was none.
void ConfigBridge::skipDaemonUnderHarness(const char* what) {
    if (!harness_said_) {
        harness_said_ = true;
        std::fprintf(stderr,
                     "vocem-config: a harness run (VOCEM_CONFIG_GEOMETRY, _SCREENSHOT or "
                     "_NO_DAEMON), so the daemon is neither started nor stopped (first "
                     "skipped: %s)\n",
                     what);
    }
}

bool ConfigBridge::startDaemon() {
    if (harness_) {
        skipDaemonUnderHarness("start");
        return true;
    }
    if (unit_available_ < 0) {
        // The probe has not answered yet: the start happens when it does.
        daemon_start_wanted_ = true;
        return true;
    }
    if (unit_available_ == 1) {
        return QProcess::startDetached(QStringLiteral("systemctl"),
                                       {QStringLiteral("--user"), QStringLiteral("start"),
                                        QStringLiteral("vocemd.service")});
    }
    const QString executable = daemonExecutable();
    if (executable.isEmpty()) {
        return false;
    }
    return QProcess::startDetached(executable, {});
}

void ConfigBridge::stopDaemon(std::function<void()> done) {
    if (harness_) {
        skipDaemonUnderHarness("stop");
        // Still asynchronous, as every other way this ends is.
        QTimer::singleShot(0, this, std::move(done));
        return;
    }
    if (unit_available_ < 0) {
        // The unit question is still open, and `pkill` below is the answer for
        // a machine with no unit -- not for a question in flight. The stop runs
        // when probeUnit settles, which its own cap bounds at three seconds.
        daemon_start_wanted_ = false;
        daemon_stop_wanted_ = std::move(done);
        return;
    }
    // One `done`, however the stop ends: by the process finishing, by it never
    // starting, or by the cap.
    auto finished = std::make_shared<bool>(false);
    const auto complete = [finished, done] {
        if (*finished) {
            return;
        }
        *finished = true;
        done();
    };
    QPointer<QProcess> stop = new QProcess(this);  // a QPointer: see probePreload
    connect(stop, &QProcess::finished, this, [stop, complete](int, QProcess::ExitStatus) {
        if (stop) {
            stop->deleteLater();
        }
        complete();
    });
    connect(stop, &QProcess::errorOccurred, this, [stop, complete] {
        if (stop) {
            stop->deleteLater();
        }
        complete();
    });
    // The unit's TimeoutStopSec is ten; past twelve the stop is not going to
    // end and the window must not hang on it.
    QTimer::singleShot(12000, this, [stop, complete] {
        if (stop && stop->state() != QProcess::NotRunning) {
            stop->kill();
        }
        complete();
    });
    if (unit_available_ == 1) {
        stop->start(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("stop"),
                                                  QStringLiteral("vocemd.service")});
        return;
    }
    // No unit to go through, so signal the process directly. SIGTERM: the daemon
    // cleans up its shared memory on the way out.
    stop->start(QStringLiteral("pkill"),
                {QStringLiteral("-TERM"), QStringLiteral("-x"), QStringLiteral("vocemd")});
}

// Quit, meaning quit. The daemon is stopped first -- systemctl waits for the
// stop, the daemon publishes cleared state before it unlinks, and the overlay
// leaves every running game within about a second (held by
// tests/daemon_notification.cpp on the backend's side) -- and only then does
// this process end, so nothing can interleave between "the window is gone" and
// "the overlay is still up" in the order a user would notice. The wait is
// asynchronous now: the windows are hidden at once and the process ends when
// the stop has, where a synchronous stop held a visible, frozen window for as
// long as the daemon took to leave -- up to its TimeoutStopSec. Reopening the
// application starts the daemon again (see the constructor), and a game still
// running reattaches to the new segment by itself.
void ConfigBridge::quitOverlay() {
    if (quitting_) {
        return;
    }
    quitting_ = true;
    for (QWindow* window : QGuiApplication::allWindows()) {
        window->hide();
    }
    stopDaemon([] { QCoreApplication::quit(); });
}

void ConfigBridge::performAction() {
    if (busy_) {
        return;
    }
    if (!attached_) {
        busy_ = true;
        announceState();
        startDaemon();
        // The daemon needs a moment to create the segment; the timer picks it up.
        QTimer::singleShot(1500, this, [this] {
            busy_ = false;
            refreshState();
        });
        return;
    }
    if (snapshot_.status == vocem::DaemonStatus::AuthorisationRefused) {
        reauthorise();
    }
}

// Discarding the token is what makes the daemon ask again on its next connection,
// so a restart is part of the operation rather than something the user must do.
void ConfigBridge::reauthorise() {
    busy_ = true;
    announceState();

    stopDaemon([this] {
        // Not under the harness: the token is the daemon's credential, and a
        // harness run without a scratch XDG_STATE_HOME would remove the real one.
        if (!harness_) {
            QFile::remove(QString::fromStdString(vocem::token_path()));
        }
        QTimer::singleShot(800, this, [this] {
            startDaemon();
            QTimer::singleShot(1500, this, [this] {
                busy_ = false;
                refreshState();
            });
        });
    });
}

QString ConfigBridge::channelName() const { return tr("Voice channel"); }

// The four people every preview draws, and never anybody real.
//
// It used to be the live channel when the daemon was connected, which made the
// preview a moving picture: the box changed width when somebody with a long name
// joined, rows appeared and disappeared under the slider being dragged, and the
// same page looked different from one minute to the next. Nothing the user is
// setting here depends on who is in the channel, and a settings window that shifts
// under the hand while it is being read is worse than one that shows an example.
//
// Fixed also means the preview can be held to the drawing: tests/panel_geometry.cpp
// builds the real panel from this same roster, and scripts/compare-preview.py puts
// the two side by side. One of each state, so every decoration a row can draw is on
// screen at once.
//
// And no pictures, which is what the measurement's roster has: each of these used
// to carry the desktop's own `user-identity` icon, drawn *over* the overlay's
// placeholder by the preview's masked-image path. Two head-and-shoulders figures
// from two artwork sets, in two greys, one of them square-shouldered -- which is
// the grey shadow under the silhouette the owner kept reporting and no
// instrument here could see: the icon does not resolve in the offscreen harness,
// so it drew nothing where it draws a second figure on a real desktop. What the
// preview shows now is the placeholder the game draws for somebody whose picture
// has not arrived, which is also the only honest example: the window has no
// Discord pictures to show.
QVariantList ConfigBridge::participants() const {
    const struct {
        const char* name;
        bool speaking;
        bool muted;
        bool deafened;
    } roster[] = {
        {"User 1", true, false, false},
        {"User 2", false, false, false},
        {"User 3", false, true, false},
        {"User 4", false, false, true},
    };

    QVariantList list;
    for (const auto& person : roster) {
        QVariantMap entry;
        entry["name"] = QString::fromUtf8(person.name);
        entry["speaking"] = person.speaking;
        entry["muted"] = person.muted;
        entry["deafened"] = person.deafened;
        list.append(entry);
    }
    return list;
}

void ConfigBridge::setPositionX(qreal value) {
    setNumber("position_x", &vocem::Config::position_x, value);
}

void ConfigBridge::setPositionY(qreal value) {
    setNumber("position_y", &vocem::Config::position_y, value);
}

void ConfigBridge::setPosition(qreal x, qreal y) {
    const float new_x = static_cast<float>(vocem::Config::clamped("position_x", x));
    const float new_y = static_cast<float>(vocem::Config::clamped("position_y", y));
    if (qFuzzyCompare(config_.position_x, new_x) && qFuzzyCompare(config_.position_y, new_y)) {
        return;
    }
    config_.position_x = new_x;
    config_.position_y = new_y;
    persist();
}

void ConfigBridge::setScale(qreal value) {
    setNumber("scale", &vocem::Config::scale, value);
}

void ConfigBridge::setOpacity(qreal value) {
    setNumber("opacity", &vocem::Config::opacity, value);
}

namespace {

// QColor carries an alpha this file has no use for: transparency is a separate
// setting, so only the RGB part is stored.
uint32_t to_rgb(const QColor& colour) {
    return static_cast<uint32_t>(colour.rgb() & 0xffffffu);
}

}  // namespace

void ConfigBridge::setPanelColour(const QColor& value) {
    const uint32_t rgb = to_rgb(value);
    if (config_.panel_colour == rgb) {
        return;
    }
    config_.panel_colour = rgb;
    persist();
}

void ConfigBridge::setNotificationColour(const QColor& value) {
    const uint32_t rgb = to_rgb(value);
    if (config_.notification_colour == rgb) {
        return;
    }
    config_.notification_colour = rgb;
    persist();
}

void ConfigBridge::setSpeakingColour(const QColor& value) {
    const uint32_t rgb = to_rgb(value);
    if (config_.speaking_colour == rgb) {
        return;
    }
    config_.speaking_colour = rgb;
    persist();
}

namespace {

// "auto", or anything QColor can read; anything else leaves the setting alone
// rather than guessing -- the file's own parser makes the same refusal.
bool parse_colour_or_auto(const QString& value, uint32_t* out) {
    if (value.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        *out = vocem::Config::kColourAuto;
        return true;
    }
    const QColor colour(value);
    if (!colour.isValid()) {
        return false;
    }
    *out = to_rgb(colour);
    return true;
}

}  // namespace

void ConfigBridge::setTextIdleColour(const QString& value) {
    uint32_t parsed = 0;
    if (!parse_colour_or_auto(value, &parsed) || config_.text_idle_colour == parsed) {
        return;
    }
    config_.text_idle_colour = parsed;
    persist();
}

void ConfigBridge::setTextSpeakingColour(const QString& value) {
    uint32_t parsed = 0;
    if (!parse_colour_or_auto(value, &parsed) || config_.text_speaking_colour == parsed) {
        return;
    }
    config_.text_speaking_colour = parsed;
    persist();
}

void ConfigBridge::setNotificationTextColour(const QString& value) {
    uint32_t parsed = 0;
    if (!parse_colour_or_auto(value, &parsed) || config_.notification_text_colour == parsed) {
        return;
    }
    config_.notification_text_colour = parsed;
    persist();
}

// What theme_for() will actually draw for the role: the ramp while the setting
// is auto, the pinned value -- nudged where it would collide with another
// measured role -- once pinned. The swatch shows this, which is what makes the
// reset honest: back on auto it reveals the ramp's own colour instead of going
// blank.
QColor ConfigBridge::effectiveTextIdleColour() const {
    const vocem::Colour colour = vocem::theme_for(config_).text_idle;
    return QColor(colour.r, colour.g, colour.b);
}

QColor ConfigBridge::effectiveTextSpeakingColour() const {
    const vocem::Colour colour = vocem::theme_for(config_).text_speaking;
    return QColor(colour.r, colour.g, colour.b);
}

QColor ConfigBridge::effectiveNotificationTextColour() const {
    const vocem::Colour colour = vocem::theme_for(config_).toast_body;
    return QColor(colour.r, colour.g, colour.b);
}

void ConfigBridge::setPanelEnabled(bool value) {
    if (config_.panel_enabled == value) {
        return;
    }
    config_.panel_enabled = value;
    persistNow(&vocem::Config::panel_enabled);
}

void ConfigBridge::setAvatarSize(qreal value) {
    setNumber("avatar_size", &vocem::Config::avatar_size, value);
}

void ConfigBridge::setAvatarIdleOpacity(qreal value) {
    setNumber("avatar_idle_opacity", &vocem::Config::avatar_idle_opacity, value);
}

void ConfigBridge::setFontSize(qreal value) {
    setNumber("font_size", &vocem::Config::font_size, value);
}

// The family, and the files it stands for. Three fields move together because
// they are one answer: the window is the only half of this project that may ask
// fontconfig anything, so what it writes has to be enough for a game to open.
//
// A family whose files cannot be resolved is refused rather than half-written.
// The alternative -- storing the name and no path -- is a setting that shows the
// user's choice in the window and draws Inter in the game, which is the shape of
// defect this project keeps finding: the interface saying one thing and the
// overlay doing another.
void ConfigBridge::setFontFamily(const QString& value) {
    const QString wanted = value.trimmed();
    if (wanted.isEmpty()) {
        if (config_.font_family.empty() && config_.font_path.empty()) {
            return;
        }
        config_.font_family.clear();
        config_.font_path.clear();
        config_.font_path_strong.clear();
        persist();
        return;
    }

    QString regular;
    QString bold;
    if (!vocem::font_file_for(wanted, false, &regular)) {
        // Refused, and said so by putting the control back: a ComboBox has
        // already moved to what was clicked, and its binding on this value only
        // re-runs when the settings change. Without this the box would go on
        // naming a family the file does not carry -- the interface saying one
        // thing while the overlay does another, which is the shape this refusal
        // exists to avoid in the first place.
        emit configChanged();
        return;
    }
    // A family whose bold the overlay cannot reach draws both weights from the
    // one file it has: ImGui has no synthetic bold, so the honest answer is one
    // weight rather than a heavier-looking lie. The channel name then leans on
    // its colour, which the palette gives it anyway.
    //
    // "Cannot reach" and not "has none": a variable family keeps its weights as
    // named instances of one file, and the overlay opens files rather than
    // instances -- so Adwaita Sans, whose bold fontconfig answers as instance 7
    // of AdwaitaSans-Regular.ttf, comes out here as the same path twice and is
    // drawn at the regular weight. The behaviour is the right one; the reason is
    // worth writing down, because a family that plainly has a bold and does not
    // show it looks like a defect from the outside.
    if (!vocem::font_file_for(wanted, true, &bold) || bold.isEmpty()) {
        bold = regular;
    }
    if (config_.font_family == wanted.toStdString() &&
        config_.font_path == regular.toStdString() &&
        config_.font_path_strong == bold.toStdString()) {
        return;
    }
    config_.font_family = wanted.toStdString();
    config_.font_path = regular.toStdString();
    config_.font_path_strong = bold.toStdString();
    persist();
}

void ConfigBridge::setKeepRunning(bool value) {
    if (config_.keep_running == value) {
        return;
    }
    config_.keep_running = value;
    persist();
}

void ConfigBridge::setTrayVoiceIcon(bool value) {
    if (config_.tray_voice_icon == value) {
        return;
    }
    config_.tray_voice_icon = value;
    persist();
}

void ConfigBridge::setStartAtLogin(bool value) {
    if (config_.start_at_login == value) {
        return;
    }
    config_.start_at_login = value;
    persist();
}

void ConfigBridge::setTextShadow(bool value) {
    if (config_.text_shadow == value) {
        return;
    }
    config_.text_shadow = value;
    persist();
}

void ConfigBridge::setShowChannelName(bool value) {
    if (config_.show_channel_name == value) {
        return;
    }
    config_.show_channel_name = value;
    persist();
}

void ConfigBridge::setOnlySpeaking(bool value) {
    if (config_.only_speaking == value) {
        return;
    }
    config_.only_speaking = value;
    persist();
}

void ConfigBridge::setHideSelf(bool value) {
    if (config_.hide_self == value) {
        return;
    }
    config_.hide_self = value;
    persist();
}

void ConfigBridge::setShowMutedState(bool value) {
    if (config_.show_muted_state == value) {
        return;
    }
    config_.show_muted_state = value;
    persist();
}

// The overlay's own palette and proportions, derived here by the same function the
// injected code calls, so the previews cannot drift from the drawing.
//
// The colours arrive with their alpha: the scrim over a muted picture and the rim
// around a badge are translucent by design, and a preview that dropped that would
// be showing a different picture rather than the same one at another size. The
// distances arrive in the overlay's reference unit, which is what the previews are
// already laid out in.
// One function builds the map whatever configuration it is asked about:
// overlayTheme() passes the window's edited copy, presetThemes() passes that
// copy with one preset's writes applied. Two hand-kept copies of this table
// would be the hand-mirrored-colours era back under another name.
static QVariantMap theme_map(const vocem::Config& config) {
    const vocem::Theme theme = vocem::theme_for(config);

    const auto colour = [](vocem::Colour c) {
        return QColor::fromRgb(c.r, c.g, c.b, c.a);
    };

    QVariantMap map;
    map["panelSurface"] = colour(theme.panel_surface);
    map["separator"] = colour(theme.separator);
    map["textChannel"] = colour(theme.text_channel);
    map["textSpeaking"] = colour(theme.text_speaking);
    map["textIdle"] = colour(theme.text_idle);
    map["textMuted"] = colour(theme.text_muted);
    map["textOverflow"] = colour(theme.text_overflow);
    map["avatarPlaceholder"] = colour(theme.avatar_placeholder);
    map["avatarMark"] = colour(theme.avatar_mark);
    map["avatarScrim"] = colour(theme.avatar_scrim);
    map["speakingRing"] = colour(theme.speaking_ring);
    map["badgeFill"] = colour(theme.badge_fill);
    map["badgeRim"] = colour(theme.badge_rim);
    map["badgeGlyph"] = colour(theme.badge_glyph);
    map["toastSurface"] = colour(theme.toast_surface);
    map["toastTitle"] = colour(theme.toast_title);
    map["toastBody"] = colour(theme.toast_body);
    map["textOutlineInk"] = colour(theme.text_outline_ink);
    // The hairline carries its alpha inside the token, already premultiplied by
    // the panel's opacity: the preview reproduces it and never re-derives it.
    map["panelHairline"] = colour(theme.panel_hairline);

    map["toastHairline"] = colour(theme.toast_hairline);
    map["toastAccent"] = colour(theme.toast_accent);
    map["toastAccentWidth"] = theme.toast_accent_width;
    map["boxRadius"] = theme.box_radius;
    map["ringOffset"] = theme.ring_offset;
    map["ringWidthFactor"] = theme.ring_width_factor;
    map["ringAllowance"] = theme.ring_allowance;
    map["separatorAccent"] = colour(theme.separator_accent);
    map["separatorAccentLength"] = theme.separator_accent_length;
    map["avatarRadiusFactor"] = theme.avatar_radius_factor;
    map["badgeOffsetFactor"] = theme.badge_offset_factor;
    map["badgeRadiusFactor"] = theme.badge_radius_factor;
    map["badgeStrokeFactor"] = theme.badge_stroke_factor;
    map["badgeRimStrokeFactor"] = theme.badge_rim_stroke_factor;
    map["badgeAllowanceFactor"] = theme.badge_allowance_factor;
    map["nameBoxPaddingX"] = theme.name_box_padding_x;
    map["nameBoxPaddingY"] = theme.name_box_padding_y;

    // Already resolved against the text-shadow switch, so a preview reproduces
    // the outline rather than re-deriving whether there is one.
    map["panelTextOutline"] = theme.panel_text_outline;
    map["toastTextOutline"] = theme.toast_text_outline;
    return map;
}

QVariantMap ConfigBridge::overlayTheme() const {
    return theme_map(config_);
}

// The theme each preset would produce if clicked, from the settings as they
// stand now. The three writes here are the same three the preset button makes
// (AppearancePage.qml): a preset is a surface, an opacity and where that surface
// is drawn, nothing else, and everything else -- pinned text colours, the
// outline switch -- passes through, so the preview shows the preset on top of
// the user's own settings.
QVariantList ConfigBridge::presetThemes() const {
    QVariantList list;
    for (const vocem::Preset& preset : vocem::kPresets) {
        vocem::Config preview = config_;
        preview.panel_colour = preset.colour;
        preview.opacity = preset.opacity;
        preview.panel_box = preset.box;
        list.append(theme_map(preview));
    }
    return list;
}

QVariantList ConfigBridge::overlayPresets() const {
    QVariantList list;
    for (const vocem::Preset& preset : vocem::kPresets) {
        QVariantMap entry;
        entry["id"] = QString::fromLatin1(preset.id);
        entry["colour"] = toColour(preset.colour);
        entry["opacity"] = preset.opacity;
        entry["box"] = preset.box;
        list.append(entry);
    }
    return list;
}

// An example, never a real message. The same reasoning as the roster above, and
// one more: a real message is somebody's private mail, and a settings window left
// open on a second monitor is not where it should turn up.
//
// No picture, for the reason the roster carries none: the example is drawn with
// the overlay's own placeholder, which is what a game draws for somebody whose
// picture has not arrived.
//
// The body is always there, because the drawn toast always carries one: the
// message's text is not a setting any more, it is a transport (vocem/note.h).
// The example the preview draws is therefore the shape of every toast.
QVariantMap ConfigBridge::notificationPreview() const {
    QVariantMap entry;
    entry["title"] = tr("User 1");
    entry["body"] = tr("sent you a direct message");
    return entry;
}

void ConfigBridge::setNotificationsEnabled(bool value) {
    if (config_.notifications_enabled == value) {
        return;
    }
    config_.notifications_enabled = value;
    persistNow(&vocem::Config::notifications_enabled);
}

// Upright or sideways. Refused rather than clamped for anything else: an
// unknown layout is a value nothing in this window can have produced, and
// silently rounding it to one of the two would make a wrong write look right.
void ConfigBridge::setPanelLayout(int value) {
    if ((value != vocem::Config::kLayoutVertical &&
         value != vocem::Config::kLayoutHorizontal) ||
        value == config_.panel_layout) {
        return;
    }
    config_.panel_layout = value;
    persist();
}

// Around everything, or behind the names. Refused rather than clamped for
// anything else, exactly as the layout is: a value neither of the two is a value
// nothing in this window can have produced, and rounding it to one of them would
// make a wrong write look right.
void ConfigBridge::setPanelBox(int value) {
    if ((value != vocem::Config::kBoxPanel && value != vocem::Config::kBoxNames) ||
        value == config_.panel_box) {
        return;
    }
    config_.panel_box = value;
    persist();
}

void ConfigBridge::setNotificationCorner(int value) {
    if (value < 0 || value > 3 || value == config_.notification_corner) {
        return;
    }
    config_.notification_corner = value;
    persist();
}

void ConfigBridge::setNotificationSeconds(qreal value) {
    setNumber("notification_seconds", &vocem::Config::notification_seconds, value);
}

void ConfigBridge::setNotificationOpacity(qreal value) {
    setNumber("notification_opacity", &vocem::Config::notification_opacity, value);
}


void ConfigBridge::setNotificationScale(qreal value) {
    setNumber("notification_scale", &vocem::Config::notification_scale, value);
}
void ConfigBridge::setNotificationMargin(qreal value) {
    setNumber("notification_margin", &vocem::Config::notification_margin, value);
}

void ConfigBridge::setScreenMargin(qreal value) {
    setNumber("screen_margin", &vocem::Config::screen_margin, value);
}
void ConfigBridge::setBoxPaddingX(qreal value) {
    setNumber("box_padding_x", &vocem::Config::box_padding_x, value);
}
void ConfigBridge::setBoxPaddingY(qreal value) {
    setNumber("box_padding_y", &vocem::Config::box_padding_y, value);
}
void ConfigBridge::setAvatarGap(qreal value) {
    setNumber("avatar_gap", &vocem::Config::avatar_gap, value);
}
void ConfigBridge::setRowSpacing(qreal value) {
    setNumber("row_spacing", &vocem::Config::row_spacing, value);
}

void ConfigBridge::setEnabled(bool value) {
    if (config_.enabled == value) {
        return;
    }
    config_.enabled = value;
    persistNow(&vocem::Config::enabled);
}
