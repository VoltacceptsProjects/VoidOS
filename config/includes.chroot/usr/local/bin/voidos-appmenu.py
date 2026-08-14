#!/usr/bin/env python3
"""
voidos-appmenu -- the VoidOS-menu equivalent of macOS's Apple menu.

A small popup Gtk.Menu (not app launching -- that's VoidDock's job) with
About/Settings/Force Quit/Lock/Restart/Shut Down/Log Out, shown from a
launcher button pinned at the left edge of the top VoidOS bar.
"""
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
import subprocess

APP_NAME = "VoidOS"


def run(cmd):
    subprocess.Popen(cmd)


def show_about(_item):
    dialog = Gtk.AboutDialog()
    dialog.set_program_name(APP_NAME)
    dialog.set_version("1.0 (Nebula)")
    dialog.set_comments("A glassmorphism-styled Linux distribution built on Debian.")
    dialog.set_logo_icon_name("start-here")
    dialog.run()
    dialog.destroy()


def build_menu():
    menu = Gtk.Menu()

    def item(label, handler):
        mi = Gtk.MenuItem(label=label)
        mi.connect("activate", handler)
        menu.append(mi)
        return mi

    item(f"About {APP_NAME}", show_about)
    menu.append(Gtk.SeparatorMenuItem())
    item("System Settings...", lambda *_: run(["xfce4-settings-manager"]))
    item("Force Quit...", lambda *_: run(["xfce4-taskmanager"]))
    menu.append(Gtk.SeparatorMenuItem())
    item("Lock Screen", lambda *_: run(["xflock4"]))
    item("Log Out...", lambda *_: run(["xfce4-session-logout", "--logout"]))
    item("Restart...", lambda *_: run(["xfce4-session-logout", "--reboot"]))
    item("Shut Down...", lambda *_: run(["xfce4-session-logout", "--halt"]))

    menu.show_all()
    return menu


def main():
    menu = build_menu()
    menu.connect("deactivate", lambda *_: Gtk.main_quit())
    menu.popup_at_pointer(None)
    Gtk.main()


if __name__ == "__main__":
    main()
