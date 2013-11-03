/*
   Copyright (C) 2013 Red Hat, Inc.

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

#ifndef _H_AS_USER_
#define _H_AS_USER_

/** AsUser runs a task as the user logged on in session_id.
 *  Constructor calls Impersonate, Destructor calls Revert, so
 *  the caller needs not worry about that.
 */
class AsUser {
public:
    ~AsUser();
    AsUser(DWORD session_id = (DWORD)-1);
    bool begin();
    void end();

private:
    DWORD _session_id;
    HANDLE _token;
    bool _started;
};

#endif
