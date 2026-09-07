// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Finding the icon of an application we only know as a running executable.
//
// There is no index from a binary back to its desktop entry: the Desktop Entry
// Specification maps an entry to a program, never the other way round, which is
// why every dock on this desktop has its own pile of heuristics for the same
// question. What is standard is what an entry contains, and this uses only that:
//
//   * Entries live in $XDG_DATA_HOME/applications and applications/ under each of
//     $XDG_DATA_DIRS (default /usr/local/share:/usr/share), user first.
//   * `TryExec` is a path to the program, absolute or found through PATH.
//   * `Exec` names the program somewhere in it -- not necessarily first: 44 of
//     this machine's 53 Game entries start with `steam`, `mangohud`, `env` or a
//     wrapper script (entry 96), and this file used to take the first word.
//   * `StartupWMClass` is the window class the application will map -- which is
//     what docks match against, and which is usually the binary's own name.
//   * `Icon` is a file when it is an absolute path, and otherwise a name to be
//     looked up through the Icon Theme Specification.
//   * `NoDisplay` and `Hidden` entries are not applications to show.
//
// On top of that, two hints the process recorded about itself when the overlay was
// loaded into it -- see vocem/apps.h. Those are worth more than any matching done
// here, because they come from the launcher rather than from a guess, but the
// systemd one is inherited by children, so an entry it names is only believed when
// that entry points back at this executable.
//
// One parser and one root enumeration for the whole project: the reading is
// apps.h's `detail::read_entry` and `detail::desktop_roots`, the same the
// injected code decides with. This file used to carry a second parser with a
// second roots list beside them, and the two had drifted apart on the one rule
// that decides whether a game's own icon is found.
//
// The lookup is indexed. It used to walk every entry for every application on
// every four-second sweep, with a QFileInfo built per candidate -- O(applications
// x entries) twice over, on a timer, for a page that is usually not open. The
// names an entry can be found by are computed once, at refresh(), into hashes;
// a lookup then examines the handful of entries that carry the name.
// `entriesExamined()` counts them, so tests/window_cost.cmake can hold the number
// rather than a clock.
//
// Nothing here knows where Steam, or anything else, is installed.

#ifndef VOCEM_DESKTOP_ENTRIES_H
#define VOCEM_DESKTOP_ENTRIES_H

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "vocem/apps.h"
#include "vocem/paths.h"

namespace vocem {

// One entry, reduced to the keys this question needs.
struct DesktopEntry {
    QString id;
    QString icon;
    QString exec;      // the whole Exec line, for the URL a launcher entry runs
    QString tryExec;
    QString wmClass;   // StartupWMClass
};

class DesktopEntries {
public:
    // Every entry on the standard search path, user directories first so that an
    // entry the user has of their own wins -- which is the rule the Base Directory
    // Specification gives for the whole path.
    void refresh() {
        entries_.clear();
        by_name_.clear();
        by_comm_.clear();
        by_steam_id_.clear();
        ++refreshes_;

        for (const std::string& root : vocem::detail::desktop_roots()) {
            const QDir directory(QString::fromStdString(root) + QStringLiteral("/applications"));
            if (!directory.exists()) {
                continue;
            }
            // Entries may sit in subdirectories, and their id then carries the path
            // with dashes for the separators.
            QDirIterator walk(directory.absolutePath(), {QStringLiteral("*.desktop")},
                              QDir::Files, QDirIterator::Subdirectories);
            while (walk.hasNext()) {
                const QString path = walk.next();
                const QString id = directory.relativeFilePath(path).replace('/', '-');
                // First one wins: the search path is in precedence order.
                if (entries_.contains(id)) {
                    continue;
                }
                DesktopEntry entry;
                if (!parse(path, id, &entry)) {
                    continue;
                }
                entries_.insert(id, entry);
                index(entry);
            }
        }
    }

    // The icon for an application, as a source QML can load: a file URL when the
    // entry names a file, and image://icon/<name> when it names an icon in the
    // theme. Empty when nothing is found, which is the caller's cue to draw
    // whatever it draws for an application it cannot put a face to.
    QString iconFor(const QString& executable, const QString& processName,
                    const QString& recordedEntry, const QString& steamAppId) const {
        const QString binary = QFileInfo(executable).fileName();
        ++lookups_;

        // 1. What the launcher said, when it said it about this process. The
        //    systemd half of that hint is inherited by children -- a game started
        //    by a launcher reports the launcher -- so it counts only if the entry
        //    it names runs this executable.
        if (!recordedEntry.isEmpty()) {
            DesktopEntry entry;
            if (byIdOrPath(recordedEntry, &entry) && !entry.icon.isEmpty()) {
                ++examined_;
                if (recordedEntry.startsWith('/') || matches(entry, binary, processName)) {
                    return source(entry.icon);
                }
            }
        }

        // 2. The entry that runs this binary, by any of the names that can name
        //    a program -- the indexed ones, so only the entries carrying the name
        //    are looked at.
        QList<QString> candidates = by_name_.value(binary.toLower());
        if (!processName.isEmpty()) {
            // The process name is what the kernel reports, cut to fifteen
            // characters, so a longer name can only be compared to that length:
            // a second index, keyed by the first fifteen of every name longer
            // than that (detail::same_name's rule).
            candidates += by_name_.value(processName.toLower());
            candidates += by_comm_.value(processName.toLower());
        }
        for (const QString& id : std::as_const(candidates)) {
            const DesktopEntry entry = entries_.value(id);
            ++examined_;
            if (!entry.icon.isEmpty() && matches(entry, binary, processName)) {
                return source(entry.icon);
            }
        }

        // 3. A game started through Steam: the id came from the game's own
        //    environment, and Steam's entry for that game runs the URL that names
        //    it. Which of the two exists is Steam's business, not ours -- if the
        //    entry was never written, this finds nothing and the caller falls back.
        if (!steamAppId.isEmpty()) {
            const QString id = by_steam_id_.value(steamAppId);
            if (!id.isEmpty()) {
                ++examined_;
                const DesktopEntry entry = entries_.value(id);
                if (!entry.icon.isEmpty()) {
                    return source(entry.icon);
                }
            }
        }

        return {};
    }

    // The instrument: how many entries every lookup so far has looked at, how
    // many lookups there were, and how many times the tree was walked.
    long entriesExamined() const { return examined_; }
    long lookups() const { return lookups_; }
    long refreshes() const { return refreshes_; }
    int size() const { return static_cast<int>(entries_.size()); }

private:
    // Whether this entry runs the program: any program word of Exec or TryExec
    // (apps.h's rule), or the window class it will map.
    static bool matches(const DesktopEntry& entry, const QString& binary,
                        const QString& processName) {
        if (binary.isEmpty() && processName.isEmpty()) {
            return false;
        }
        const std::string binary_std = binary.toStdString();
        const std::string comm = processName.toStdString();
        if (vocem::detail::exec_names(entry.exec.toStdString(), binary_std, comm) ||
            vocem::detail::exec_names(entry.tryExec.toStdString(), binary_std, comm)) {
            return true;
        }
        if (!entry.wmClass.isEmpty()) {
            const QString name = QFileInfo(entry.wmClass).fileName();
            if (name.compare(binary, Qt::CaseInsensitive) == 0 ||
                vocem::detail::same_name(name.toStdString().c_str(), comm)) {
                return true;
            }
        }
        return false;
    }

    bool byIdOrPath(const QString& reference, DesktopEntry* out) const {
        if (reference.startsWith('/')) {
            return parse(reference, QFileInfo(reference).fileName(), out);
        }
        const QString id =
            reference.endsWith(QStringLiteral(".desktop")) ? reference
                                                           : reference + QStringLiteral(".desktop");
        const auto found = entries_.constFind(id);
        if (found == entries_.constEnd()) {
            return false;
        }
        *out = found.value();
        return true;
    }

    static QString source(const QString& icon) {
        if (icon.startsWith('/')) {
            return QUrl::fromLocalFile(icon).toString();
        }
        return QIcon::hasThemeIcon(icon) ? QStringLiteral("image://icon/") + icon : QString();
    }

    // apps.h's reader: O_NOFOLLOW, regular files only, a size cap, the plain keys
    // of the [Desktop Entry] group. False for a file that is not an entry and
    // for an entry that is not an application to show.
    static bool parse(const QString& path, const QString& id, DesktopEntry* out) {
        const vocem::detail::Entry read = vocem::detail::read_entry(path.toStdString());
        if (!read.found || read.no_display) {
            return false;
        }
        out->id = id;
        out->icon = QString::fromStdString(read.icon);
        out->exec = QString::fromStdString(read.exec);
        out->tryExec = QString::fromStdString(read.try_exec);
        out->wmClass = QString::fromStdString(read.wm_class);
        return true;
    }

    // Every name this entry can be found by, lower-cased, into the two hashes;
    // the Steam entry's app id into the third.
    void index(const DesktopEntry& entry) {
        QStringList names;
        for (const std::string& name :
             vocem::detail::exec_program_names(entry.exec.toStdString())) {
            names << QString::fromStdString(name);
        }
        for (const std::string& name :
             vocem::detail::exec_program_names(entry.tryExec.toStdString())) {
            names << QString::fromStdString(name);
        }
        if (!entry.wmClass.isEmpty()) {
            names << QFileInfo(entry.wmClass).fileName();
        }
        for (const QString& name : std::as_const(names)) {
            const QString key = name.toLower();
            if (key.isEmpty()) {
                continue;
            }
            QList<QString>& ids = by_name_[key];
            if (!ids.contains(entry.id)) {
                ids.append(entry.id);
            }
            if (key.size() > static_cast<int>(vocem::kCommLength)) {
                QList<QString>& cut = by_comm_[key.left(static_cast<int>(vocem::kCommLength))];
                if (!cut.contains(entry.id)) {
                    cut.append(entry.id);
                }
            }
        }
        static const QString prefix = QStringLiteral("steam://rungameid/");
        const qsizetype at = entry.exec.indexOf(prefix);
        if (at >= 0) {
            QString steam_id;
            for (qsizetype i = at + prefix.size(); i < entry.exec.size(); ++i) {
                if (!entry.exec.at(i).isDigit()) {
                    break;
                }
                steam_id.append(entry.exec.at(i));
            }
            if (!steam_id.isEmpty() && !by_steam_id_.contains(steam_id)) {
                by_steam_id_.insert(steam_id, entry.id);
            }
        }
    }

    QHash<QString, DesktopEntry> entries_;
    // Lower-cased program name -> the ids of the entries that carry it; the
    // same for the first fifteen characters of every name longer than that; and
    // Steam's app id -> the one entry that runs its URL.
    QHash<QString, QList<QString>> by_name_;
    QHash<QString, QList<QString>> by_comm_;
    QHash<QString, QString> by_steam_id_;
    mutable long examined_ = 0;
    mutable long lookups_ = 0;
    long refreshes_ = 0;
};

}  // namespace vocem

#endif  // VOCEM_DESKTOP_ENTRIES_H
