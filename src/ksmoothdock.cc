/*
 * This file is part of KSmoothDock.
 * Copyright (C) 2015 Viet Dang (dangvd@gmail.com)
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

#include "ksmoothdock.h"

#include <cstdlib>
#include <iostream>
#include <utility>

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDir>
#include <QDesktopWidget>
#include <QPainter>
#include <QProcess>
#include <QStringList>

#include <KAboutData>
#include <KLocalizedString>
#include <KWindowSystem>
#include <netwm_def.h>

#include "launcher.h"

namespace ksmoothdock {

KSmoothDock::KSmoothDock()
    : QWidget(),
      isEntering_(false),
      isLeaving_(false),
      isAnimationActive_(false),
      aboutDialog_(KAboutData::applicationData(), this) {
  setAttribute(Qt::WA_TranslucentBackground);
  KWindowSystem::setType(winId(), NET::Dock);
  setMouseTracking(true);
  desktopWidth_ = QApplication::desktop()->screenGeometry().width();
  desktopHeight_ = QApplication::desktop()->screenGeometry().height();
  launchersRelativePath_ = ".ksmoothdock/launchers";
  launchersPath_ = QDir::homePath() + "/" + launchersRelativePath_;
  animationTimer_.reset(new QTimer(this));
  connect(animationTimer_.get(), SIGNAL(timeout()), this, 
      SLOT(updateAnimation()));
  createMenu();
  loadConfig();
}

KSmoothDock::~KSmoothDock() {}

void KSmoothDock::init() {
  initLaunchers();
  initLayoutVars();
  updateLayout();
  setStrut();
}

void KSmoothDock::resize(int w, int h) {
  QWidget::resize(w, h);
  int x, y;
  if (position_ == PanelPosition::Top) {
    x = (desktopWidth_ - w) / 2;
    y = 0;
  } else if (position_ == PanelPosition::Bottom) {
    x = (desktopWidth_ - w) / 2;
    y = desktopHeight_ - h;
  } else if (position_ == PanelPosition::Left) {
    x = 0;
    y = (desktopHeight_ - h) / 2;
  } else {  // Right
    x = desktopWidth_ - w;
    y = (desktopHeight_ - h) / 2;
  }
  move(x, y);
}

void KSmoothDock::openLaunchersDir() {
  QProcess::startDetached("dolphin " + launchersPath_);
}

void KSmoothDock::reload() {
  items_.clear();
  init();
  repaint();
}

void KSmoothDock::setPosition(PanelPosition position) {
  position_ = position;
  orientation_ = (position_ == PanelPosition::Top ||
      position_ == PanelPosition::Bottom)
      ? Qt::Horizontal : Qt::Vertical;
  positionTop_->setChecked(position == PanelPosition::Top);
  positionBottom_->setChecked(position == PanelPosition::Bottom);
  positionLeft_->setChecked(position == PanelPosition::Left);
  positionRight_->setChecked(position == PanelPosition::Right);
}

void KSmoothDock::updateAnimation() {
  for (const auto& item : items_) {
    item->nextAnimationStep();
  }
  ++currentAnimationStep_;
  backgroundLength_ = startBackgroundLength_
      + (endBackgroundLength_ - startBackgroundLength_)
          * currentAnimationStep_ / numAnimationSteps_;
  if (currentAnimationStep_ == numAnimationSteps_) {
    animationTimer_->stop();
    isAnimationActive_ = false;
    if (isLeaving_) {
      isLeaving_ = false;
      updateLayout();
    }
  }
  repaint();
}

void KSmoothDock::resetCursor() {
  setCursor(QCursor(Qt::ArrowCursor));
}

void KSmoothDock::about() {
  aboutDialog_.show();
}

void KSmoothDock::paintEvent(QPaintEvent* e) {
  QPainter painter(this);

  if (isHorizontal()) {
    const int y = (position_ == PanelPosition::Top) ? 0 : height() - minHeight_;
    painter.fillRect((width() - backgroundLength_) / 2, y, 
        backgroundLength_, minHeight_, backgroundColor_);

    painter.setPen(borderColor_);
    painter.drawRect((width() - backgroundLength_) / 2, y, 
        backgroundLength_ - 1, minHeight_ - 1);
  } else {  // Vertical
    const int x =  (position_ == PanelPosition::Left) ? 0 : width() - minWidth_;
    painter.fillRect(x, (height() - backgroundLength_) / 2,
        minWidth_, backgroundLength_, backgroundColor_);

    painter.setPen(borderColor_);
    painter.drawRect(x, (height() - backgroundLength_) / 2,
        minWidth_ - 1, backgroundLength_ - 1);
  }

  for (int i = 0; i < items_.size(); ++i) {
    items_[i]->draw(&painter);
  }
}

void KSmoothDock::mouseMoveEvent ( QMouseEvent* e) {
  if (isAnimationActive_) {
    return;
  }

  int i = findActiveItem(e->x(), e->y());
  if (i < 0 || i >= items_.size()) {
    tooltip_.hide();
  } else {
    showTooltip(i);
  }

  updateLayout(e->x(), e->y());
}

void KSmoothDock::mousePressEvent(QMouseEvent* e) {
  if (isAnimationActive_) {
    return;
  }

  if (e->button() == Qt::LeftButton) {
    int i = findActiveItem(e->x(), e->y());
    if (i < 0 || i >= items_.size()) {
      return;
    }
    Launcher* launcher = static_cast<Launcher*>(items_[i].get());
    if (!launcher->isCommandInternal()) {  // acknowledge launching the program.
      showWaitCursor();
    }
    items_[i]->mousePressEvent(e);
  } else if (e->button() == Qt::RightButton) {
    menu_.popup(e->globalPos());
  }
}

void KSmoothDock::enterEvent (QEvent* e) {
  isEntering_ = true;
}

void KSmoothDock::leaveEvent(QEvent* e) {
  isLeaving_ = true;
  updateLayout();
  tooltip_.hide();
}

void KSmoothDock::createMenu() {
  menu_.addAction(i18n("Edit &Launchers"), this, SLOT(openLaunchersDir()));

  QMenu* position = menu_.addMenu(i18n("Panel &Position"));
  positionTop_ = position->addAction(i18n("&Top"), this,
      SLOT(setPositionTop()));
  positionTop_->setCheckable(true);
  positionBottom_ = position->addAction(i18n("&Bottom"), this,
      SLOT(setPositionBottom()));
  positionBottom_->setCheckable(true);
  positionLeft_ = position->addAction(i18n("&Left"), this,
      SLOT(setPositionLeft()));
  positionLeft_->setCheckable(true);
  positionRight_ = position->addAction(i18n("&Right"), this,
      SLOT(setPositionRight()));
  positionRight_->setCheckable(true);

  menu_.addSeparator();
  menu_.addAction(i18n("&Reload"), this, SLOT(reload()));
  menu_.addSeparator();
  menu_.addAction(i18n("&About KSmoothDock"), this, SLOT(about()));
  menu_.addSeparator();
  menu_.addAction(i18n("E&xit"), this, SLOT(close()));
}

void KSmoothDock::loadConfig() {
  setPosition(PanelPosition::Bottom);
  minSize_ = kDefaultMinSize;
  maxSize_ = kDefaultMaxSize;
  backgroundColor_.setNamedColor("#638abd");
  backgroundColor_.setAlphaF(0.42);
  borderColor_.setNamedColor("#b1c4de");
  tooltipFontSize_ = 18;
}

void KSmoothDock::initLaunchers() {
  if (!loadLaunchers()) {
    createDefaultLaunchers();
    QDir::home().mkpath(launchersRelativePath_);
    saveLaunchers();
  }
}

bool KSmoothDock::loadLaunchers() {
  if (!QDir::home().exists(launchersRelativePath_)) {
    return false;
  }
  QDir launchersDir(launchersPath_);
  QStringList files = launchersDir.entryList(QDir::Files, QDir::Name);
  if (files.isEmpty()) {
    return false;
  }
  for (int i = 0; i < files.size(); ++i) {
    const QString& file = launchersPath_ + "/" + files.at(i);
    items_.push_back(std::unique_ptr<DockItem>(
        new Launcher(file, orientation_, minSize_, maxSize_)));
  }
}

void KSmoothDock::createDefaultLaunchers() {
  const int kNumItems = 7;
  const char* const kItems[kNumItems][3] = {
    // Name, icon name, command.
    {"Home Folder", "system-file-manager", "dolphin"},
    {"Show Desktop", "user-desktop", kShowDesktopCommand},
    {"Terminal", "utilities-terminal", "konsole"},
    {"Text Editor", "kate", "kate"},
    {"Web Browser", "applications-internet", "firefox"},
    {"Audio Player", "audio-headphones", "amarok"},
    {"System Settings", "preferences-desktop", "systemsettings5"}
  };
  for (int i = 0; i < kNumItems; ++i) {
    items_.push_back(std::unique_ptr<DockItem>(
      new Launcher(kItems[i][0], orientation_, kItems[i][1], minSize_,
          maxSize_, kItems[i][2])));
  }
}

void KSmoothDock::saveLaunchers() {
  for (int i = 0; i < items_.size(); ++i) {
    Launcher* launcher = dynamic_cast<Launcher*>(items_[i].get());
    if (launcher != nullptr) {
      launcher->saveToFile(launchersPath_ + "/"
          + QString::number(i + 1) + " - " + items_[i]->label_ + ".desktop");
    }
  }
}

void KSmoothDock::initLayoutVars() {
  itemSpacing_ = minSize_ / 2;
  parabolicMaxX_ = static_cast<int>(2.5 * (minSize_ + itemSpacing_));
  numAnimationSteps_ = 20;
  animationSpeed_ = 16;

  tooltip_.setFontSize(tooltipFontSize_);
  tooltip_.setFontBold(true);
  tooltip_.setFontItalic(true);
  tooltip_.setFontColor(Qt::white);
  tooltip_.setBackgroundColor(Qt::black);

  const int distance = minSize_ + itemSpacing_;
  minWidth_ = items_.size() * distance;
  minHeight_ = distance;
  if (items_.size() >= 5) {
    maxWidth_ = parabolic(0) + 2 * parabolic(distance) +
	2 * parabolic(2 * distance) - 5 * minSize_ + minWidth_;
  } else if (items_.size() == 4) {
    maxWidth_ = parabolic(0) + 2 * parabolic(distance) +
	parabolic(2 * distance) - 4 * minSize_ + minWidth_;
  } else if (items_.size() == 3) {
    maxWidth_ = parabolic(0) + 2 * parabolic(distance) -
	3 * minSize_ + minWidth_;
  } else if (items_.size() == 2) {
    maxWidth_ = parabolic(0) + parabolic(distance) -
	2 * minSize_ + minWidth_;
  } else if (items_.size() == 1) {
    maxWidth_ = parabolic(0) - minSize_ + minWidth_;
  }
  maxHeight_ = itemSpacing_ + maxSize_;

  if (orientation_ == Qt::Vertical) {
    std::swap(minWidth_, minHeight_);
    std::swap(maxWidth_, maxHeight_);
  }
}

void KSmoothDock::updateLayout() {
  if (isLeaving_) {
    for (const auto& item : items_) {
      item->setAnimationStartAsCurrent();
      startBackgroundLength_ = isHorizontal() ? maxWidth_ : maxHeight_;
    }
  }

  for (int i = 0; i < items_.size(); ++i) {
    items_[i]->size_ = minSize_;
    if (isHorizontal()) {
      items_[i]->left_ = itemSpacing_ / 2 + i * (minSize_ + itemSpacing_);
      items_[i]->top_ = itemSpacing_ / 2;
      items_[i]->minCenter_ = items_[i]->left_ + minSize_ / 2;
    } else {  // Vertical
      items_[i]->left_ = itemSpacing_ / 2;
      items_[i]->top_ = itemSpacing_ / 2 + i * (minSize_ + itemSpacing_);
      items_[i]->minCenter_ = items_[i]->top_ + minSize_ / 2;
    }
  }
  backgroundLength_ = isHorizontal() ? minWidth_ : minHeight_;

  if (isLeaving_) {
    for (const auto& item : items_) {
      item->endSize_ = item->size_;
      if (isHorizontal()) {
        item->endLeft_ = item->left_ + (maxWidth_ - minWidth_) / 2;
        if (position_ == PanelPosition::Top) {
          item->endTop_ = item->top_;
        } else {  // Bottom
          item->endTop_ = item->top_ + (maxHeight_ - minHeight_);
        }
      } else {  // Vertical
        item->endTop_ = item->top_ + (maxHeight_ - minHeight_) / 2;
        if (position_ == PanelPosition::Left) {
          item->endLeft_ = item->left_;
        } else {  // Right
          item->endLeft_ = item->left_ + (maxWidth_ - minWidth_);
        }
      }
      item->startAnimation(numAnimationSteps_);
    }
    endBackgroundLength_ = isHorizontal() ? minWidth_ : minHeight_;
    backgroundLength_ = startBackgroundLength_;
    currentAnimationStep_ = 0;
    isAnimationActive_ = true;
    animationTimer_->start(32 - animationSpeed_);
  } else {
    resize(minWidth_, minHeight_);
  }
}

void KSmoothDock::updateLayout(int x, int y) {
  if (isEntering_) {
    for (const auto& item : items_) {
      item->startSize_ = item->size_;
      if (isHorizontal()) {
        item->startLeft_ = item->left_ + (maxWidth_ - minWidth_) / 2;
        if (position_ == PanelPosition::Top) {
          item->startTop_ = item->top_;
        } else {  // Bottom
          item->startTop_ = item->top_ + (maxHeight_ - minHeight_);
        }
      } else {  // Vertical
        item->startTop_ = item->top_ + (maxHeight_ - minHeight_) / 2;
        if (position_ == PanelPosition::Left) {
          item->startLeft_ = item->left_;
        } else {  // Right
          item->startLeft_ = item->left_ + (maxWidth_ - minWidth_);
        }
      }
    }
    startBackgroundLength_ = isHorizontal() ? minWidth_ : minHeight_;
  }

  int first_update_index = -1;
  int last_update_index = 0;
  if (isHorizontal()) {
    items_[0]->left_ = itemSpacing_ / 2;
  } else {  // Vertical
    items_[0]->top_ = itemSpacing_ / 2;
  }
  for (int i = 0; i < items_.size(); ++i) {
    int delta;
    if (isHorizontal()) {
      delta = abs(items_[i]->minCenter_ - x + (width() - minWidth_) / 2);
    } else {  // Vertical
      delta = abs(items_[i]->minCenter_ - y + (height() - minHeight_) / 2);
    }
    if (delta < parabolicMaxX_) {
      if (first_update_index == -1) {
        first_update_index = i;
      }
      last_update_index = i;
    }
    items_[i]->size_ = parabolic(delta);
    if (position_ == PanelPosition::Top) {
      items_[i]->top_ = itemSpacing_ / 2;
    } else if (position_ == PanelPosition::Bottom) {
      items_[i]->top_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getHeight();
    } else if (position_ == PanelPosition::Left) {
      items_[i]->left_ = itemSpacing_ / 2;
    } else {  // Right
      items_[i]->left_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getWidth();
    }
    if (i > 0) {
      if (isHorizontal()) {
        items_[i]->left_ = items_[i - 1]->left_ + items_[i - 1]->getWidth()
            + itemSpacing_;
      } else {  // Vertical
        items_[i]->top_ = items_[i - 1]->top_ + items_[i - 1]->getHeight()
            + itemSpacing_;
      }
    }
  }
  for (int i = last_update_index + 1; i < items_.size(); ++i) {
    if (isHorizontal()) {
      items_[i]->left_ = maxWidth_
          - (items_.size() - i) * (minSize_ + itemSpacing_) + itemSpacing_ / 2;
    } else {  // Vertical
      items_[i]->top_ = maxHeight_
          - (items_.size() - i) * (minSize_ + itemSpacing_) + itemSpacing_ / 2;
    }
  }
  if (first_update_index == 0 && last_update_index < items_.size() - 1) {
    for (int i = last_update_index; i >= first_update_index; --i) {
      if (isHorizontal()) {
        items_[i]->left_ = items_[i + 1]->left_ - items_[i]->getWidth()
            - itemSpacing_;
      } else {  // Vertical
        items_[i]->top_ = items_[i + 1]->top_ - items_[i]->getHeight()
            - itemSpacing_;
      }
    }
  }

  if (isEntering_) {
    for (const auto& item : items_) {
      item->setAnimationEndAsCurrent();
      item->startAnimation(numAnimationSteps_);
    }
    endBackgroundLength_ = isHorizontal() ? maxWidth_ : maxHeight_;
    backgroundLength_ = startBackgroundLength_;
    currentAnimationStep_ = 0;
    isAnimationActive_ = true;
    isEntering_ = false;
    animationTimer_->start(32 - animationSpeed_);
  }

  resize(maxWidth_, maxHeight_);
  repaint();
}

void KSmoothDock::setStrut() {
  if (position_ == PanelPosition::Top) {
    KWindowSystem::setStrut(winId(), 0, 0, height(), 0);
  } else if (position_ == PanelPosition::Bottom) {
    KWindowSystem::setStrut(winId(), 0, 0, 0, height());
  } else if (position_ == PanelPosition::Left) {
    KWindowSystem::setStrut(winId(), width(), 0, 0, 0);
  } else {  // Right
    KWindowSystem::setStrut(winId(), 0, width(), 0, 0);
  }
}

int KSmoothDock::findActiveItem(int x, int y) {
  int i = 0;
  while (i < items_.size() &&
      ((orientation_ == Qt::Horizontal && items_[i]->left_ < x) ||
      (orientation_ == Qt::Vertical && items_[i]->top_ < y))) {
    ++i;
  }
  return i - 1;
}

void KSmoothDock::showTooltip(int i) {
  tooltip_.setText(items_[i]->label_);
  int x, y;
  if (position_ == PanelPosition::Top) {
    x = (desktopWidth_ - width()) / 2 + items_[i]->left_
        - tooltip_.width() / 2 + items_[i]->getWidth() / 2;
    y = maxHeight_ + kTooltipSpacing;
  } else if (position_ == PanelPosition::Bottom) {
    x = (desktopWidth_ - width()) / 2 + items_[i]->left_
        - tooltip_.width() / 2 + items_[i]->getWidth() / 2;
    // No need for additional tooltip spacing in this position.
    y = desktopHeight_ - maxHeight_ - tooltip_.height();
  } else if (position_ == PanelPosition::Left) {
    x = maxWidth_ + kTooltipSpacing;
    y = (desktopHeight_ - height()) / 2 + items_[i]->top_
        - tooltip_.height() / 2 + items_[i]->getHeight() / 2;
  } else {  // Right
    x = desktopWidth_ - maxWidth_ - tooltip_.width() - kTooltipSpacing;
    y = (desktopHeight_ - height()) / 2 + items_[i]->top_
        - tooltip_.height() / 2 + items_[i]->getHeight() / 2;
  }
  tooltip_.move(x, y);
  tooltip_.show();
}

void KSmoothDock::showWaitCursor() {
  setCursor(QCursor(Qt::WaitCursor));
  QTimer::singleShot(1000 /* msecs */, this, SLOT(resetCursor()));
}

int KSmoothDock::parabolic(int x) {
  // Assume x >= 0.
  if (x > parabolicMaxX_) {
    return minSize_;
  } else {
    return maxSize_ -
        (x * x * (maxSize_ - minSize_)) / (parabolicMaxX_ * parabolicMaxX_);
  }
}

}  // namespace ksmoothdock
