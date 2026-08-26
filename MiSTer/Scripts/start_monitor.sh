#!/bin/bash
# MiSTer Monitor — MiSTer-side server
# Copyright (C) 2025-2026 chipster6502
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with this program.  If not, see
# <https://www.gnu.org/licenses/>.

#
# Compatibility shim. This script was replaced by MiSTer_Monitor.sh; it stays
# so auto-start lines written by earlier versions keep working. Running it with
# no arguments migrates that line, after which this file can be dropped from
# the database.
#

exec bash /media/fat/Scripts/MiSTer_Monitor.sh "$@"
