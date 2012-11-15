# Slideshow Shuffle Plugin for Eye of MATE
# Copyright (C) 2008  Johannes Marbach <jm@rapidrabbit.de>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

import eom, random

class SlideshowShufflePlugin(eom.Plugin):
    def __init__(self):
        eom.Plugin.__init__(self)

    def activate(self, window):
        random.seed()
        self.slideshow = False
        self.state_handler_id = \
            window.connect('window-state-event', self.state_changed_cb)

    def deactivate(self, window):
        window.disconnect(self.state_handler_id)

    def state_changed_cb(self, window, data = None):
        mode = window.get_mode()

        if mode == eom.WindowMode(3) and not self.slideshow:
            # Slideshow starts
            self.slideshow = True

            # Query position of current image
            if not window.get_image():
                pos = 0
            else:
                pos = window.get_store().get_pos_by_image(window.get_image())

            # Generate random map
            uri_list = [row[2].get_uri_for_display() \
                        for row in window.get_store()]
            self.map = {uri_list.pop(pos): 0}
            supply = list(range(1, len(uri_list) + 1))
            for uri in uri_list:
                self.map[uri] = supply.pop(random.randint(0, len(supply) - 1))

            # Put random sort function in place
            window.get_store().\
                set_default_sort_func(self.random_sort_function)
        elif mode == eom.WindowMode(1) and self.slideshow:
            # Slideshow ends
            self.slideshow = False

            # Put alphabetic sort function in place
            window.get_store().\
                set_default_sort_func(self.alphabetic_sort_function)

    def random_sort_function(self, store, iter1, iter2, data = None):
        pos1 = self.map[store[iter1][2].get_uri_for_display()]
        pos2 = self.map[store[iter2][2].get_uri_for_display()]

        if  pos1 > pos2:
            return 1
        elif pos1 < pos2:
            return -1
        else:
            return 0

    def alphabetic_sort_function(self, store, iter1, iter2, data = None):
        uri1 = store[iter1][2].get_uri_for_display().lower()
        uri2 = store[iter2][2].get_uri_for_display().lower()

        if uri1 > uri2:
            return 1
        elif uri1 < uri2:
            return -1
        else:
            return 0
