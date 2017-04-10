/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _H_DESKTOP_LAYOUT
#define _H_DESKTOP_LAYOUT

#include <vector>
#include "vdcommon.h"

class DisplayMode {
public:
    DisplayMode(LONG pos_x, LONG pos_y, DWORD width, DWORD height, DWORD depth, bool attached)
        : _pos_x (pos_x)
        , _pos_y (pos_y)
        , _width (width)
        , _height (height)
        , _depth (depth)
        , _attached (attached)
    {
        _primary = (pos_x == 0 && pos_y == 0 && attached);
    }

    LONG get_pos_x() const { return _pos_x;}
    LONG get_pos_y() const { return _pos_y;}
    DWORD get_width() const { return _width;}
    DWORD get_height() const { return _height;}
    DWORD get_depth() const { return _depth;}
    bool get_attached() const { return _attached;}
    void set_pos(LONG x, LONG y) { _pos_x = x; _pos_y = y;}
    void move_pos(LONG x, LONG y) { _pos_x += x; _pos_y += y;}
    void set_res(DWORD width, DWORD height, DWORD depth);
    void set_depth(DWORD depth) { _depth = depth;}
    void set_attached(bool attached) { _attached = attached;}
    bool is_primary() { return _primary; }

private:
    LONG _pos_x;
    LONG _pos_y;
    DWORD _width;
    DWORD _height;
    DWORD _depth;
    bool _attached;
    bool _primary;

    friend class DesktopLayout;
};

typedef std::vector<DisplayMode*> Displays;
class DisplayConfig;

class DesktopLayout {
public:
    DesktopLayout();
    ~DesktopLayout();
    void get_displays();
    void set_displays();
    void lock() { _mutex.lock(); }
    void unlock() { _mutex.unlock(); }
    DisplayMode* get_display(int i) { return _displays.at(i);}
    size_t get_display_count() { return _displays.size();}
    DWORD get_total_width() { return _total_width;}
    DWORD get_total_height() { return _total_height;}
    void set_position_configurable(bool flag);
private:
    void clean_displays();
    void normalize_displays_pos();
    DisplayMode * get_primary_display();
    bool init_dev_mode(LPCTSTR dev_name, DEVMODE* dev_mode, DisplayMode* mode);
    static bool consistent_displays();
    static bool is_attached(LPCTSTR dev_name);
    static bool get_qxl_device_id(WCHAR* device_key, DWORD* device_id);
private:
    mutex_t _mutex;
    Displays _displays;
    DWORD _total_width;
    DWORD _total_height;
    DisplayConfig* _display_config;
};

#endif
