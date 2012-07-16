/*
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AutoType.h"

#include <QtCore/QPluginLoader>
#include <QtGui/QApplication>

#include "autotype/AutoTypePlatformPlugin.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/ListDeleter.h"
#include "core/Tools.h"

AutoType* AutoType::m_instance = Q_NULLPTR;

AutoType::AutoType(QObject* parent)
    : QObject(parent)
    , m_inAutoType(false)
    , m_pluginLoader(new QPluginLoader(this))
    , m_plugin(Q_NULLPTR)
    , m_executor(Q_NULLPTR)
{
    // prevent crash when the plugin has unresolved symbols
    m_pluginLoader->setLoadHints(QLibrary::ResolveAllSymbolsHint);

    // TODO: scan in proper paths
#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
    m_pluginLoader->setFileName(QCoreApplication::applicationDirPath() + "/autotype/x11/libkeepassx-autotype-x11.so");
#endif

    QObject* pluginInstance = m_pluginLoader->instance();
    if (pluginInstance) {
        m_plugin = qobject_cast<AutoTypePlatformInterface*>(pluginInstance);
        if (m_plugin) {
            m_executor = m_plugin->createExecutor();
            connect(pluginInstance, SIGNAL(globalShortcutTriggered()), SIGNAL(globalShortcutTriggered()));
        }
    }

    if (!m_plugin) {
        qWarning("Unable to load auto-type plugin:\n%s", qPrintable(m_pluginLoader->errorString()));
    }
}

AutoType::~AutoType()
{
    delete m_executor;
}

AutoType* AutoType::instance()
{
    if (!m_instance) {
        m_instance = new AutoType(qApp);
    }

    return m_instance;
}

QStringList AutoType::windowTitles()
{
    if (!m_plugin) {
        return QStringList();
    }

    return m_plugin->windowTitles();
}

void AutoType::performAutoType(const Entry* entry, QWidget* hideWindow, const QString& customSequence)
{
    if (m_inAutoType || !m_plugin) {
        return;
    }
    m_inAutoType = true;

    QString sequence;
    if (customSequence.isEmpty()) {
        sequence = entry->resolvePlaceholders(entry->autoTypeSequence());
    }
    else {
        sequence = customSequence;
    }

    QList<AutoTypeAction*> actions;
    ListDeleter<AutoTypeAction*> actionsDeleter(&actions);

    if (!parseActions(sequence, entry, actions)) {
        m_inAutoType = false; // TODO: make this automatic
        return;
    }

    if (hideWindow) {
        hideWindow->showMinimized();
    }

    Tools::wait(500);

    WId activeWindow = m_plugin->activeWindow();

    Q_FOREACH (AutoTypeAction* action, actions) {
        if (m_plugin->activeWindow() != activeWindow) {
            qWarning("Active window changed, interrupting auto-type.");
            break;
        }

        action->accept(m_executor);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    m_inAutoType = false;
}

void AutoType::performGlobalAutoType(const QList<Database*>& dbList)
{
    if (m_inAutoType || !m_plugin) {
        return;
    }

    QString windowTitle = m_plugin->activeWindowTitle();

    if (windowTitle.isEmpty()) {
        return;
    }

    m_inAutoType = true;

    QList<QPair<Entry*, QString> > entryList;

    Q_FOREACH (Database* db, dbList) {
        Q_FOREACH (Entry* entry, db->rootGroup()->entriesRecursive()) {
            QString sequence = entry->autoTypeSequence(windowTitle);
            if (!sequence.isEmpty()) {
                entryList << QPair<Entry*, QString>(entry, sequence);
            }
        }
    }

    if (entryList.isEmpty()) {
        m_inAutoType = false;
    }
    else if (entryList.size() == 1) {
        m_inAutoType = false;
        performAutoType(entryList.first().first, Q_NULLPTR, entryList.first().second);
    }
    else {
        // TODO: implement
        m_inAutoType = false;
    }
}

bool AutoType::registerGlobalShortcut(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    Q_ASSERT(key);
    Q_ASSERT(modifiers);

    if (!m_plugin) {
        return false;
    }

    if (key != m_currentGlobalKey || modifiers != m_currentGlobalModifiers) {
        if (m_currentGlobalKey && m_currentGlobalModifiers) {
            m_plugin->unregisterGlobalShortcut(m_currentGlobalKey, m_currentGlobalModifiers);
        }

        if (m_plugin->registerGlobalShortcut(key, modifiers)) {
            m_currentGlobalKey = key;
            m_currentGlobalModifiers = modifiers;
            return true;
        }
        else {
            return false;
        }
    }
    else {
        return true;
    }
}

void AutoType::unregisterGlobalShortcut()
{
    if (m_plugin && m_currentGlobalKey && m_currentGlobalModifiers) {
        m_plugin->unregisterGlobalShortcut(m_currentGlobalKey, m_currentGlobalModifiers);
    }
}

int AutoType::callEventFilter(void* event)
{
    if (!m_plugin) {
        return -1;
    }

    return m_plugin->platformEventFilter(event);
}

bool AutoType::parseActions(const QString& sequence, const Entry* entry, QList<AutoTypeAction*>& actions)
{
    QString tmpl;
    bool inTmpl = false;

    Q_FOREACH (const QChar& ch, sequence) {
        // TODO: implement support for {{}, {}} and {DELAY=X}

        if (inTmpl) {
            if (ch == '{') {
                qWarning("Syntax error in auto-type sequence.");
                return false;
            }
            else if (ch == '}') {
                actions.append(createActionFromTemplate(tmpl, entry));
                inTmpl = false;
                tmpl.clear();
            }
            else {
                tmpl += ch;
            }
        }
        else if (ch == '{') {
            inTmpl = true;
        }
        else if (ch == '}') {
            qWarning("Syntax error in auto-type sequence.");
            return false;
        }
        else {
            actions.append(new AutoTypeChar(ch));
        }
    }

    return true;
}

QList<AutoTypeAction*> AutoType::createActionFromTemplate(const QString& tmpl, const Entry* entry)
{
    QString tmplName = tmpl.toLower();
    int num = -1;
    QList<AutoTypeAction*> list;

    QRegExp repeatRegEx("(.+) (\\d+)", Qt::CaseSensitive, QRegExp::RegExp2);
    if (repeatRegEx.exactMatch(tmplName)) {
        tmplName = repeatRegEx.cap(1);
        num = repeatRegEx.cap(2).toInt();

        if (num == 0) {
            return list;
        }
        // some safety checks
        else if (tmplName == "delay") {
            if (num > 10000) {
                return list;
            }
        }
        else if (num > 100) {
            return list;
        }
    }

    if (tmplName == "tab") {
        list.append(new AutoTypeKey(Qt::Key_Tab));
    }
    else if (tmplName == "enter") {
        list.append(new AutoTypeKey(Qt::Key_Enter));
    }
    else if (tmplName == "up") {
        list.append(new AutoTypeKey(Qt::Key_Up));
    }
    else if (tmplName == "down") {
        list.append(new AutoTypeKey(Qt::Key_Down));
    }
    else if (tmplName == "left") {
        list.append(new AutoTypeKey(Qt::Key_Left));
    }
    else if (tmplName == "right") {
        list.append(new AutoTypeKey(Qt::Key_Right));
    }
    else if (tmplName == "insert" || tmplName == "ins") {
        list.append(new AutoTypeKey(Qt::Key_Insert));
    }
    else if (tmplName == "delete" || tmplName == "del") {
        list.append(new AutoTypeKey(Qt::Key_Delete));
    }
    else if (tmplName == "home") {
        list.append(new AutoTypeKey(Qt::Key_Home));
    }
    else if (tmplName == "end") {
        list.append(new AutoTypeKey(Qt::Key_End));
    }
    else if (tmplName == "pgup") {
        list.append(new AutoTypeKey(Qt::Key_PageUp));
    }
    else if (tmplName == "pgdown") {
        list.append(new AutoTypeKey(Qt::Key_PageDown));
    }
    else if (tmplName == "backspace" || tmplName == "bs" || tmplName == "bksp") {
        list.append(new AutoTypeKey(Qt::Key_Backspace));
    }
    else if (tmplName == "break") {
        list.append(new AutoTypeKey(Qt::Key_Pause));
    }
    else if (tmplName == "capslock") {
        list.append(new AutoTypeKey(Qt::Key_CapsLock));
    }
    else if (tmplName == "esc") {
        list.append(new AutoTypeKey(Qt::Key_Escape));
    }
    else if (tmplName == "help") {
        list.append(new AutoTypeKey(Qt::Key_Help));
    }
    else if (tmplName == "numlock") {
        list.append(new AutoTypeKey(Qt::Key_NumLock));
    }
    else if (tmplName == "ptrsc") {
        list.append(new AutoTypeKey(Qt::Key_Print));
    }
    else if (tmplName == "scolllock") {
        list.append(new AutoTypeKey(Qt::Key_ScrollLock));
    }
    // Qt doesn't know about keypad keys so use the normal ones instead
    else if (tmplName == "add" || tmplName == "+") {
        list.append(new AutoTypeChar('+'));
    }
    else if (tmplName == "subtract") {
        list.append(new AutoTypeChar('-'));
    }
    else if (tmplName == "multiply") {
        list.append(new AutoTypeChar('*'));
    }
    else if (tmplName == "divide") {
        list.append(new AutoTypeChar('/'));
    }
    else if (tmplName == "^") {
        list.append(new AutoTypeChar('^'));
    }
    else if (tmplName == "%") {
        list.append(new AutoTypeChar('%'));
    }
    else if (tmplName == "~") {
        list.append(new AutoTypeChar('~'));
    }
    else if (tmplName == "(") {
        list.append(new AutoTypeChar('('));
    }
    else if (tmplName == ")") {
        list.append(new AutoTypeChar(')'));
    }
    else if (tmplName == "{") {
        list.append(new AutoTypeChar('{'));
    }
    else if (tmplName == "}") {
        list.append(new AutoTypeChar('}'));
    }
    else {
        QRegExp fnRegexp("f(\\d+)", Qt::CaseInsensitive, QRegExp::RegExp2);
        if (fnRegexp.exactMatch(tmplName)) {
            int fnNo = fnRegexp.cap(1).toInt();
            if (fnNo >= 1 && fnNo <= 16) {
                list.append(new AutoTypeKey(static_cast<Qt::Key>(Qt::Key_F1 - 1 + fnNo)));
            }
        }
    }

    if (!list.isEmpty()) {
        for (int i = 1; i < num; i++) {
            list.append(list.at(0)->clone());
        }

        return list;
    }


    if (tmplName == "delay" && num > 0) {
        list.append(new AutoTypeDelay(num));
    }
    else if (tmplName == "clearfield") {
        list.append(new AutoTypeClearField());
    }

    if (!list.isEmpty()) {
        return list;
    }


    QString placeholder = QString("{%1}").arg(tmplName);
    QString resolved = entry->resolvePlaceholders(placeholder);
    if (placeholder != resolved) {
        Q_FOREACH (const QChar& ch, resolved) {
            list.append(new AutoTypeChar(ch));
        }
    }

    return list;
}
