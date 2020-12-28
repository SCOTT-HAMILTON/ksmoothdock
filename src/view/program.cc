/*
 * This file is part of KSmoothDock.
 * Copyright (C) 2019 Viet Dang (dangvd@gmail.com)
 *
 * KSmoothDock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KSmoothDock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KSmoothDock.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "program.h"

#include <iostream>

#include <QGuiApplication>
#include <QProcess>
#include <QTimer>

#include <KDesktopFile>
#include <KLocalizedString>
#include <KMessageBox>
#include <KWindowSystem>

#include "dock_panel.h"
#include <utils/command_utils.h>
#include <utils/draw_utils.h>

namespace ksmoothdock {

Program::Program(DockPanel* parent, MultiDockModel* model, const QString& label,
    Qt::Orientation orientation, const QString& iconName, int minSize,
    int maxSize, const QString& command, const QString& taskCommand, bool pinned)
    : IconBasedDockItem(parent, label, orientation, iconName, minSize, maxSize),
      model_(model),
      command_(command),
      taskCommand_(taskCommand),
      launching_(false),
      pinned_(pinned),
      demandsAttention_(false),
      attentionStrong_(false) {
  createMenu();

  animationTimer_.setInterval(500);
  connect(&animationTimer_, &QTimer::timeout, this, [this]() {
    attentionStrong_ = !attentionStrong_;
    parent_->update();
  });
}

void Program::draw(QPainter *painter) const {
  if (launching_ || (!tasks_.empty() && active()) || attentionStrong_) {
    drawHighlightedIcon(model_->backgroundColor(), left_, top_, getWidth(), getHeight(),
                        5, size_ / 8, painter);
  } else if (!tasks_.empty()) {
    drawHighlightedIcon(model_->backgroundColor(), left_, top_, getWidth(), getHeight(),
                        5, size_ / 8, painter, 0.25);
  }
  IconBasedDockItem::draw(painter);
}

void Program::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) { // Run the application.
    if (command_ == kShowDesktopCommand) {
      KWindowSystem::setShowingDesktop(!KWindowSystem::showingDesktop());
    } else if (isCommandLockScreen(command_)) {
      parent_->leaveEvent(nullptr);
      QTimer::singleShot(500, []() {
        lockScreen();
      });
    } else {
      if (tasks_.empty()) {
        launch();
      } else {
        const auto mod = QGuiApplication::keyboardModifiers();
        if (mod & Qt::ShiftModifier) {
          launch();
        } else {
          const auto activeTask = getActiveTask();
          if (activeTask >= 0) {
            if (tasks_.size() == 1) {
              KWindowSystem::minimizeWindow(tasks_[0].wId);
            } else {
              // Cycles through tasks.
              auto nextTask = (activeTask < static_cast<int>(tasks_.size() - 1)) ?
                    (activeTask + 1) : 0;
              KWindowSystem::forceActiveWindow(tasks_[nextTask].wId);
            }
          } else {
            for (unsigned i = 0; i < tasks_.size(); ++i) {
              KWindowSystem::forceActiveWindow(tasks_[i].wId);
            }
          }
        }
      }
    }
  } else if (e->button() == Qt::RightButton) {
    menu_.popup(e->globalPos());
  }
}

QString Program::getLabel() const {
  const unsigned taskCount = tasks_.size();
  return (taskCount > 1) ?
      label_ + " (" + QString::number(tasks_.size()) + " instances)" :
      label_;
}

bool Program::addTask(const TaskInfo& task) {
  if (areTheSameCommand(taskCommand_, task.command)) {
    tasks_.push_back(ProgramTask(task.wId, task.name, task.demandsAttention));
    if (task.demandsAttention) {
      setDemandsAttention(true);
    }
    return true;
  }
  return false;
}

bool Program::updateTask(const TaskInfo& task) {
  if (!areTheSameCommand(taskCommand_, task.command)) {
    return false;
  }

  for (auto& existingTask : tasks_) {
    if (existingTask.wId == task.wId) {
      existingTask.demandsAttention = task.demandsAttention;
      updateDemandsAttention();
      return true;
    }
  }

  return false;
}

bool Program::removeTask(WId wId) {
  for (int i = 0; i < static_cast<int>(tasks_.size()); ++i) {
    if (tasks_[i].wId == wId) {
      tasks_.erase(tasks_.begin() + i);
      return true;
    }
  }
  return false;
}

bool Program::hasTask(WId wId) {
  for (const auto& task : tasks_) {
    if (task.wId == wId) {
      return true;
    }
  }
  return false;
}

bool Program::beforeTask(const QString& command) {
  return taskCommand_ < command;
}

void Program::launch() {
  launch(command_);
  parent_->showWaitCursor();
  setLaunching(true);
  parent_->update();
  QTimer::singleShot(500, [this]() {
    setLaunching(false);
    parent_->update();
  });
}

void Program::pinUnpin() {
  pinned_ = !pinned_;
  if (pinned_) {
    model_->addLauncher(parent_->dockId(), LauncherConfig(label_, iconName_, command_));
  } else {  // !pinned
    model_->removeLauncher(parent_->dockId(), command_);
    if (shouldBeRemoved()) {
      parent_->delayedRefresh();
    }
  }
}

// Code from Qt 6.0 QProcess::splitCommand
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
QStringList Program::splitCommand(QStringView command)
{
  QStringList args;
  QString tmp;
  int quoteCount = 0;
  bool inQuote = false;

  // handle quoting. tokens can be surrounded by double quotes
  // "hello world". three consecutive double quotes represent
  // the quote character itself.
  for (int i = 0; i < command.size(); ++i) {
    if (command.at(i) == QLatin1Char('"')) {
      ++quoteCount;
      if (quoteCount == 3) {
        // third consecutive quote
        quoteCount = 0;
        tmp += command.at(i);
      }
      continue;
    }
    if (quoteCount) {
      if (quoteCount == 1)
        inQuote = !inQuote;
      quoteCount = 0;
    }
    if (!inQuote && command.at(i).isSpace()) {
      if (!tmp.isEmpty()) {
        args += tmp;
        tmp.clear();
      }
    } else {
      tmp += command.at(i);
    }
  }
  if (!tmp.isEmpty())
    args += tmp;

  return args;
}
#endif

void Program::launch(const QString& commandLine) {
  // Code from Qt 6.0 QProcess::startCommand
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  QStringList args = splitCommand(commandLine);
#else
  QStringList args = QProcess::splitCommand(commandLine);
#endif
  const QString program = args.takeFirst();
  if (!QProcess::startDetached(program, args)) {
    KMessageBox::error(nullptr,
        i18n("Could not run command: ") + commandLine);
  }
}

void Program::createMenu() {
  menu_.addAction(QIcon::fromTheme("configure"), i18n("Edit &Launchers"), parent_,
                  [this] { parent_->showEditLaunchersDialog(); });

  if (model_->showTaskManager(parent_->dockId())) {
    menu_.addAction(QIcon::fromTheme("configure"),
                    i18n("Task Manager &Settings"),
                    parent_,
                    [this] { parent_->showTaskManagerSettingsDialog(); });
  }

  if (!isCommandInternal(command_) && !isCommandDBus(command_)) {
    menu_.addAction(QIcon::fromTheme("list-add"), i18n("&New Instance"), this,
                    [this] { launch(); });
  }

  pinAction_ = menu_.addAction(
      i18n("Pinned"), this,
      [this] {
        pinUnpin();
      });
  pinAction_->setCheckable(true);
  pinAction_->setChecked(pinned_);

  menu_.addSeparator();
  parent_->addPanelSettings(&menu_);
}

void Program::setDemandsAttention(bool value) {
  if (demandsAttention_ == value) {
    return;
  }

  demandsAttention_ = value;
  if (demandsAttention_) {
    animationTimer_.start();
  } else if (animationTimer_.isActive()) {
    animationTimer_.stop();
  }
}

void Program::updateDemandsAttention() {
  for (const auto& task : tasks_) {
    if (task.demandsAttention) {
      setDemandsAttention(true);
      return;
    }
  }
  setDemandsAttention(false);
}

}  // namespace ksmoothdock
