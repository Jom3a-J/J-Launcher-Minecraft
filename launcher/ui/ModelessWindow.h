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

#pragma once

#include <QDialog>
#include <QPointer>

#include <type_traits>
#include <utility>

namespace UI::Modeless {

void show(QDialog* dialog);
void activate(QWidget* window);

template <typename Window, typename Factory>
Window* showOrActivate(QPointer<Window>& slot, Factory&& factory)
{
    if (!slot) {
        slot = std::forward<Factory>(factory)();
    }

    if (slot) {
        if constexpr (std::is_base_of_v<QDialog, Window>) {
            show(slot.data());
        } else {
            activate(slot.data());
        }
    }
    return slot.data();
}

}  // namespace UI::Modeless
