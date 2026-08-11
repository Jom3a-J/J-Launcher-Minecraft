// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
 *  Copyright (C) 2026 J Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "ModelessWindow.h"

namespace UI::Modeless {

void show(QDialog* dialog)
{
    dialog->setModal(false);
    dialog->setWindowModality(Qt::NonModal);
    activate(dialog);
}

void activate(QWidget* window)
{
    if (window->isMinimized()) {
        window->showNormal();
    }
    window->show();
    window->raise();
    window->activateWindow();
}

}  // namespace UI::Modeless
