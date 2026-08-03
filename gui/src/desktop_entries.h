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
//   * `Exec` starts with the program, followed by arguments and field codes.
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
// Nothing here knows where Steam, or anything else, is installed.

#ifndef VOCEM_DESKTOP_ENTRIES_H
#define VOCEM_DESKTOP_ENTRIES_H

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

namespace vocem {

// One entry, reduced to the keys this question needs.
struct DesktopEntry {
    QString id;
    QString icon;
    QString executable;   // TryExec, or the program part of Exec
    QString wmClass;      // StartupWMClass
    QString exec;         // the whole Exec line, for the URL a launcher entry runs
};

class DesktopEntries {
public:
    // Every entry on the standard search path, user directories first so that an
    // entry the user has of their own wins -- which is the rule the Base Directory
    // Specification gives for the whole path.
    void refresh() {
        entries_.clear();

        QStringList roots;
        const QByteArray home = qgetenv("XDG_DATA_HOME");
        roots << (home.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share")
                                 : QString::fromLocal8Bit(home));
        const QByteArray dirs = qgetenv("XDG_DATA_DIRS");
        const QString list = dirs.isEmpty() ? QStringLiteral("/usr/local/share:/usr/share")
                                            : QString::fromLocal8Bit(dirs);
        roots << list.split(':', Qt::SkipEmptyParts);

        for (const QString& root : std::as_const(roots)) {
            const QDir directory(root + QStringLiteral("/applications"));
            if (!directory.exists()) {
                continue;
            }
            // Entries may sit in subdirectories, and their id then carries the path
            // with dashes for the separators.
            QDirIterator walk(directory.absolutePath(), {QStringLiteral("*.desktop")},
                              QDir::Files, QDirIterator::Subdirectories);
            while (walk.hasNext()) {
                const QString path = walk.next();
                DesktopEntry entry = parse(path);
                if (entry.id.isEmpty()) {
                    continue;
                }
                entry.id = directory.relativeFilePath(path).replace('/', '-');
                // First one wins: the search path is in precedence order.
                if (!entries_.contains(entry.id)) {
                    entries_.insert(entry.id, entry);
                }
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

        // 1. What the launcher said, when it said it about this process. The
        //    systemd half of that hint is inherited by children -- a game started
        //    by a launcher reports the launcher -- so it counts only if the entry
        //    it names runs this executable.
        if (!recordedEntry.isEmpty()) {
            const DesktopEntry entry = byIdOrPath(recordedEntry);
            if (!entry.icon.isEmpty() &&
                (recordedEntry.startsWith('/') || matches(entry, binary, processName))) {
                return source(entry.icon);
            }
        }

        // 2. The entry that runs this binary, by any of the three keys that name
        //    a program.
        for (const DesktopEntry& entry : entries_) {
            if (!entry.icon.isEmpty() && matches(entry, binary, processName)) {
                return source(entry.icon);
            }
        }

        // 3. A game started through Steam: the id came from the game's own
        //    environment, and Steam's entry for that game runs the URL that names
        //    it. Which of the two exists is Steam's business, not ours -- if the
        //    entry was never written, this finds nothing and the caller falls back.
        if (!steamAppId.isEmpty()) {
            const QString url = QStringLiteral("steam://rungameid/") + steamAppId;
            for (const DesktopEntry& entry : entries_) {
                if (!entry.icon.isEmpty() && entry.exec.contains(url)) {
                    return source(entry.icon);
                }
            }
        }

        return {};
    }

private:
    static bool matches(const DesktopEntry& entry, const QString& binary,
                        const QString& processName) {
        if (binary.isEmpty() && processName.isEmpty()) {
            return false;
        }
        for (const QString& candidate : {entry.executable, entry.wmClass}) {
            if (candidate.isEmpty()) {
                continue;
            }
            const QString name = QFileInfo(candidate).fileName();
            // The process name is what the kernel reports, cut to fifteen
            // characters, so a long binary can only be compared to that length.
            if (name.compare(binary, Qt::CaseInsensitive) == 0 ||
                (!processName.isEmpty() &&
                 name.left(processName.size()).compare(processName, Qt::CaseInsensitive) == 0 &&
                 name.size() <= 15 + 1)) {
                return true;
            }
        }
        return false;
    }

    DesktopEntry byIdOrPath(const QString& reference) const {
        if (reference.startsWith('/')) {
            return parse(reference);
        }
        const QString id =
            reference.endsWith(QStringLiteral(".desktop")) ? reference
                                                           : reference + QStringLiteral(".desktop");
        return entries_.value(id);
    }

    static QString source(const QString& icon) {
        if (icon.startsWith('/')) {
            return QUrl::fromLocalFile(icon).toString();
        }
        return QIcon::hasThemeIcon(icon) ? QStringLiteral("image://icon/") + icon : QString();
    }

    // Only the [Desktop Entry] group, and only the plain keys: a localised Icon[xx]
    // is a different key and not one to be picked up by accident.
    static DesktopEntry parse(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        DesktopEntry entry;
        entry.id = QFileInfo(path).fileName();

        QTextStream stream(&file);
        bool inside = false;
        while (!stream.atEnd()) {
            const QString line = stream.readLine().trimmed();
            if (line.startsWith('[')) {
                inside = line == QStringLiteral("[Desktop Entry]");
                continue;
            }
            if (!inside || line.startsWith('#')) {
                continue;
            }
            const int equals = line.indexOf('=');
            if (equals < 0) {
                continue;
            }
            const QString key = line.left(equals).trimmed();
            const QString value = line.mid(equals + 1).trimmed();
            if (key == QStringLiteral("Icon")) {
                entry.icon = value;
            } else if (key == QStringLiteral("TryExec")) {
                entry.executable = value;
            } else if (key == QStringLiteral("Exec")) {
                entry.exec = value;
                if (entry.executable.isEmpty()) {
                    // The program is the first token, and it may be quoted.
                    QString program = value.section(' ', 0, 0);
                    if (program.startsWith('"') && program.endsWith('"')) {
                        program = program.mid(1, program.size() - 2);
                    }
                    entry.executable = program;
                }
            } else if (key == QStringLiteral("StartupWMClass")) {
                entry.wmClass = value;
            } else if (key == QStringLiteral("NoDisplay") || key == QStringLiteral("Hidden")) {
                if (value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
                    return {};  // not an application to show
                }
            }
        }
        return entry;
    }

    QHash<QString, DesktopEntry> entries_;
};

}  // namespace vocem

#endif  // VOCEM_DESKTOP_ENTRIES_H
