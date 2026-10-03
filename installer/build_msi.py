"""Build the XP Code Windows Installer package: dist\\XPCode-<version>.msi

Uses only Python's standard msilib module, which talks to Windows Installer (msi.dll) directly,
so no WiX or .NET is needed and it runs on Windows XP with Python 3.4.

    build.bat                         (build\\xpcode.exe must exist)
    python installer\\build_msi.py

The package installs per machine into "Program Files\\XP Code", adds a Start Menu shortcut, an
optional desktop shortcut and an Add/Remove Programs entry, and upgrades older versions in place.
"""
import msilib
import os
import re
import shutil
import uuid
from msilib import Dialog, schema, sequence, add_data, add_tables

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Both generated once with uuid.uuid4() (OS cryptographic RNG). Never change them: the UpgradeCode
# identifies the product line across versions, and NAMESPACE seeds every other GUID below.
UPGRADE_CODE = "{362CBF46-001C-405E-B2C0-EDC51C8E3A2B}"
NAMESPACE = uuid.UUID("dcc728dd-8ddb-4e6a-89a0-bb8a33d1e99a")
MANUFACTURER = "Peter Warrington (lilpete.me)"


def guid(name):
    """Name-based UUIDv5 under NAMESPACE: unique because the namespace is random, and stable so that
    rebuilding the same version yields the same ProductCode and component GUIDs (each version gets
    its own ProductCode, which is what major upgrades require). The package code is random per build."""
    return "{%s}" % str(uuid.uuid5(NAMESPACE, name)).upper()


def app_version():
    with open(os.path.join(ROOT, "src", "xpcode.h")) as f:
        return re.search(r'#define APP_VERSION "(\d+\.\d+\.\d+)"', f.read()).group(1)


def stage_files(stage):
    exe = os.path.join(ROOT, "build", "xpcode.exe")
    if not os.path.exists(exe):
        raise SystemExit("build\\xpcode.exe not found - run build.bat first")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    shutil.copy(exe, stage)
    shutil.copy(os.path.join(ROOT, "LICENSE"), os.path.join(stage, "LICENSE.txt"))


def ensure_actions(db, table, actions):
    """Add standard actions msilib's default sequence tables may lack."""
    view = db.OpenView("SELECT `Action` FROM `%s`" % table)
    view.Execute(None)
    have = set()
    while True:
        try:
            rec = view.Fetch()
        except msilib.MSIError:      # Python 3.4 raises at the end of the result set
            break
        if not rec:
            break
        have.add(rec.GetString(1))
    view.Close()
    add_data(db, table, [a for a in actions if a[0] not in have])


def set_key_path(db, component, file_key):
    view = db.OpenView("UPDATE `Component` SET `KeyPath` = '%s' WHERE `Component` = '%s'" % (file_key, component))
    view.Execute(None)
    view.Close()


# ---------------------------------------------------------------- user interface

W, H = 370, 270


def buttons(dlg, back, nxt, cancel, next_text="&Next >", first="Back"):
    """Standard bottom row; returns (back, next, cancel). Tab order must form one cycle, so Cancel
    leads back to the dialog's first control."""
    dlg.line("BottomLine", 0, 234, W, 0)
    b = dlg.pushbutton("Back", 180, 243, 56, 17, 3 if back else 1, "< &Back", "Next")
    n = dlg.pushbutton("Next", 236, 243, 56, 17, 3 if nxt else 1, next_text, "Cancel")
    c = dlg.pushbutton("Cancel", 304, 243, 56, 17, 3 if cancel else 1, "Cancel", first)
    if cancel:
        c.event("SpawnDialog", "CancelDlg")
    return b, n, c


def header(dlg, title, subtitle):
    dlg.text("Title", 15, 8, 290, 15, 0x30003, "{\\TitleFont}" + title)
    dlg.text("Subtitle", 25, 23, 280, 15, 0x30003, subtitle)
    dlg.control("Logo", "Icon", 326, 6, 32, 32, 1 | 0x100000 | 0x400000, None, "AppIcon", None, None)
    dlg.line("HeaderLine", 0, 44, W, 0)


def add_ui(db, version):
    add_data(db, "TextStyle", [("DlgFont8", "Tahoma", 8, None, 0),
                               ("DlgFontBold8", "Tahoma", 8, None, 1),
                               ("TitleFont", "Tahoma", 9, None, 1),
                               ("BigTitleFont", "Tahoma", 13, None, 1)])
    add_data(db, "Property", [("DefaultUIFont", "DlgFont8"), ("ErrorDialog", "ErrorDlg")])
    add_data(db, "UIText", [("AbsentPath", None), ("bytes", "bytes"), ("GB", "GB"), ("KB", "KB"), ("MB", "MB"),
                            ("NewFolder", "Folder|New Folder"), ("SelAbsentAbsent", None), ("TimeRemaining",
                            "Time remaining: {[1] min }{[2] sec}")])
    ensure_actions(db, "InstallUISequence", [
        ("WelcomeDlg", "NOT Installed", 1230),
        ("MaintenanceDlg", "Installed AND NOT RESUME AND NOT Preselected", 1250),
        ("ProgressDlg", None, 1280),
        ("ExitDialog", None, -1), ("UserExit", None, -2), ("FatalError", None, -3)])

    title = "[ProductName] Setup"

    # Welcome
    d = Dialog(db, "WelcomeDlg", 50, 50, W, H, 3, title, "Next", "Next", "Cancel")
    d.text("Title", 20, 20, 300, 40, 0x30003, "{\\BigTitleFont}Welcome to the [ProductName] %s Setup Wizard" % version)
    d.control("Logo", "Icon", 320, 20, 32, 32, 1 | 0x100000 | 0x400000, None, "AppIcon", None, None)
    d.text("Body", 20, 72, 330, 80, 0x30003,
           "This wizard will install [ProductName], a code editor with an integrated terminal, "
           "on your computer.\r\n\r\nClick Next to continue or Cancel to exit Setup.")
    b, n, c = buttons(d, False, True, True)
    n.event("NewDialog", "InstallDirDlg")

    # Install folder and options
    d = Dialog(db, "InstallDirDlg", 50, 50, W, H, 3, title, "Folder", "Next", "Cancel")
    header(d, "Destination Folder", "Choose where to install [ProductName].")
    d.text("FolderLabel", 20, 60, 320, 12, 3, "&Install [ProductName] to:")
    d.control("Folder", "PathEdit", 20, 74, 330, 18, 3, "INSTALLDIR", None, "Desktop", None)
    d.checkbox("Desktop", 20, 104, 330, 14, 3, "DESKTOPSHORTCUT", "Create a &desktop shortcut", "Back")
    d.text("SpaceNote", 20, 130, 330, 30, 3, "Settings are stored per user in Application Data\\XP Code.")
    b, n, c = buttons(d, True, True, True, "&Install", first="Folder")
    b.event("NewDialog", "WelcomeDlg")
    n.event("SetTargetPath", "INSTALLDIR", ordering=1)
    n.event("EndDialog", "Return", ordering=2)

    # Maintenance (run again when already installed)
    d = Dialog(db, "MaintenanceDlg", 50, 50, W, H, 3, title, "Next", "Next", "Cancel")
    header(d, "Remove [ProductName]", "[ProductName] is already installed on this computer.")
    d.text("Body", 20, 60, 330, 40, 3, "Click Remove to uninstall [ProductName] from this computer.")
    b, n, c = buttons(d, False, True, True, "&Remove")
    n.event("Remove", "ALL", ordering=1)
    n.event("EndDialog", "Return", ordering=2)

    # Progress (modeless)
    d = Dialog(db, "ProgressDlg", 50, 50, W, H, 1, title, "Cancel", "Cancel", "Cancel")
    header(d, "Installing [ProductName]", "Please wait while Setup configures [ProductName].")
    d.text("Status", 20, 65, 330, 12, 3, "")
    d.control("ActionText", "Text", 20, 80, 330, 12, 3, None, "", None, None).mapping("ActionText", "Text")
    d.control("Progress", "ProgressBar", 20, 100, 330, 12, 65537, None, "Progress", None, None).mapping("SetProgress", "Progress")
    d.line("BottomLine", 0, 234, W, 0)
    d.pushbutton("Back", 180, 243, 56, 17, 1, "< &Back", None)
    d.pushbutton("Next", 236, 243, 56, 17, 1, "&Next >", None)
    d.pushbutton("Cancel", 304, 243, 56, 17, 3, "Cancel", None).event("SpawnDialog", "CancelDlg")

    # Finish
    d = Dialog(db, "ExitDialog", 50, 50, W, H, 3, title, "Finish", "Finish", "Finish")
    d.text("Title", 20, 20, 300, 40, 0x30003, "{\\BigTitleFont}Completed the [ProductName] Setup Wizard")
    d.control("Logo", "Icon", 320, 20, 32, 32, 1 | 0x100000 | 0x400000, None, "AppIcon", None, None)
    d.text("Body", 20, 72, 330, 40, 0x30003, "Click Finish to exit Setup.")
    launch = d.checkbox("Launch", 20, 110, 330, 14, 3, "LAUNCHAPP", "&Launch [ProductName] now", "Finish")
    launch.condition("Hide", 'REMOVE="ALL"')
    d.line("BottomLine", 0, 234, W, 0)
    f = d.pushbutton("Finish", 236, 243, 56, 17, 3, "&Finish", "Launch")
    f.event("EndDialog", "Return", ordering=1)
    f.event("DoAction", "LaunchApp", 'LAUNCHAPP="1" AND NOT REMOVE="ALL"', ordering=2)

    for name, text in (("UserExit", "[ProductName] Setup was interrupted. Your system has not been modified."),
                       ("FatalError", "[ProductName] Setup ended prematurely because of an error. "
                                      "Your system has not been modified.")):
        d = Dialog(db, name, 50, 50, W, H, 3, title, "Finish", "Finish", "Finish")
        d.text("Title", 20, 20, 300, 40, 0x30003, "{\\BigTitleFont}[ProductName] Setup")
        d.text("Body", 20, 72, 330, 40, 0x30003, text)
        d.line("BottomLine", 0, 234, W, 0)
        d.pushbutton("Finish", 236, 243, 56, 17, 3, "&Finish", None).event("EndDialog", "Exit")

    d = Dialog(db, "CancelDlg", 50, 10, 260, 85, 3, title, "No", "No", "No")
    d.text("Text", 48, 15, 194, 30, 3, "Are you sure you want to cancel [ProductName] setup?")
    d.pushbutton("Yes", 72, 57, 56, 17, 3, "&Yes", "No").event("EndDialog", "Exit")
    d.pushbutton("No", 132, 57, 56, 17, 3, "&No", "Yes").event("EndDialog", "Return")

    # Windows Installer reports errors through a dialog with these exact control names
    d = Dialog(db, "ErrorDlg", 50, 10, 330, 101, 65543, title, "ErrorText", None, None)
    d.control("ErrorIcon", "Icon", 15, 9, 24, 24, 1 | 0x100000 | 0x200000, None, "AppIcon", None, None)
    d.text("ErrorText", 50, 9, 280, 48, 3, "")
    for i, (btn, label, result) in enumerate((("A", "&Abort", "Abort"), ("C", "&Cancel", "Cancel"),
                                               ("I", "&Ignore", "Ignore"), ("N", "&No", "No"), ("O", "&OK", "Ok"),
                                               ("R", "&Retry", "Retry"), ("Y", "&Yes", "Yes"))):
        d.pushbutton(btn, 6 + i * 46, 72, 44, 17, 3, label, None).event("EndDialog", "Error" + result)


# ---------------------------------------------------------------- package

def main():
    version = app_version()
    dist = os.path.join(ROOT, "dist")
    stage = os.path.join(dist, "stage")
    stage_files(stage)
    msi_path = os.path.join(dist, "XPCode-%s.msi" % version)
    if os.path.exists(msi_path):
        os.remove(msi_path)

    db = msilib.init_database(msi_path, schema, "XP Code", guid("product " + version), version, MANUFACTURER)
    add_tables(db, sequence)
    si = db.GetSummaryInformation(10)
    si.SetProperty(msilib.PID_TITLE, "XP Code %s Installer" % version)
    si.SetProperty(msilib.PID_SUBJECT, "XP Code")
    si.SetProperty(msilib.PID_COMMENTS, "A code editor with an integrated terminal, in plain Win32.")
    si.SetProperty(msilib.PID_KEYWORDS, "Installer, XP Code, editor")
    si.Persist()

    icon = os.path.join(ROOT, "res", "icon.ico")
    add_data(db, "Binary", [("AppIcon", msilib.Binary(icon))])
    add_data(db, "Icon", [("xpcode.ico", msilib.Binary(icon))])
    add_data(db, "Property", [
        ("UpgradeCode", UPGRADE_CODE), ("ALLUSERS", "1"), ("ARPPRODUCTICON", "xpcode.ico"),
        ("ARPNOMODIFY", "1"), ("ARPNOREPAIR", "1"), ("DESKTOPSHORTCUT", "1"), ("LAUNCHAPP", "1"),
        ("SecureCustomProperties", "OLDERFOUND;NEWERFOUND;DESKTOPSHORTCUT")])

    # replace older versions; refuse to downgrade a newer one
    add_data(db, "Upgrade", [(UPGRADE_CODE, "0.0.0", version, None, 0x100, None, "OLDERFOUND"),
                             (UPGRADE_CODE, version, None, None, 0x2, None, "NEWERFOUND")])
    add_data(db, "LaunchCondition", [("NOT NEWERFOUND", "A newer version of [ProductName] is already installed.")])
    std = [("FindRelatedProducts", None, 25), ("MigrateFeatureStates", None, 1200),
           ("RemoveExistingProducts", None, 1401), ("RemoveShortcuts", None, 3200), ("RemoveFolders", None, 3600),
           ("CreateFolders", None, 3700), ("CreateShortcuts", None, 4500), ("RemoveRegistryValues", None, 2600),
           ("WriteRegistryValues", None, 5000), ("RemoveFiles", None, 3500)]
    ensure_actions(db, "InstallExecuteSequence", std)
    ensure_actions(db, "InstallUISequence", [("FindRelatedProducts", None, 25)])

    # directories and files
    cab = msilib.CAB("xpcode")
    root = msilib.Directory(db, cab, None, stage, "TARGETDIR", "SourceDir")
    pf = msilib.Directory(db, cab, root, stage, "ProgramFilesFolder", "PFiles")
    feature = msilib.Feature(db, "Complete", "XP Code", "The XP Code editor.", 1, directory="INSTALLDIR")
    feature.set_current()
    inst = msilib.Directory(db, cab, pf, stage, "INSTALLDIR", "XPCODE|XP Code")
    # Python 3.4's start_component(keyfile=...) is broken, so key paths are set afterwards
    inst.start_component("XPCode", flags=0, uuid=guid("component xpcode.exe"))
    set_key_path(db, "XPCode", inst.add_file("xpcode.exe", version=version + ".0", language="1033"))
    inst.add_file("LICENSE.txt")
    cab.commit(db)

    # shortcuts
    add_data(db, "Directory", [("ProgramMenuFolder", "TARGETDIR", "."),
                               ("XPCodeMenu", "ProgramMenuFolder", "XPCODE|XP Code"),
                               ("DesktopFolder", "TARGETDIR", "."),
                               ("PersonalFolder", "TARGETDIR", ".")])
    add_data(db, "Component", [("DesktopShortcut", guid("component desktop shortcut"), "INSTALLDIR", 4,
                                'DESKTOPSHORTCUT="1"', "DesktopShortcutReg")])
    add_data(db, "FeatureComponents", [("Complete", "DesktopShortcut")])
    add_data(db, "Registry", [("DesktopShortcutReg", 2, "Software\\XP Code", "DesktopShortcut", "#1", "DesktopShortcut")])
    add_data(db, "Shortcut", [
        ("StartMenuShortcut", "XPCodeMenu", "XPCODE|XP Code", "XPCode", "[INSTALLDIR]xpcode.exe", None,
         "Code editor with an integrated terminal", None, None, None, None, "PersonalFolder"),
        ("DesktopShortcutLnk", "DesktopFolder", "XPCODE|XP Code", "DesktopShortcut", "[INSTALLDIR]xpcode.exe", None,
         "Code editor with an integrated terminal", None, None, None, None, "PersonalFolder")])
    add_data(db, "RemoveFile", [("RemoveMenuFolder", "XPCode", None, "XPCodeMenu", 2)])
    add_data(db, "CustomAction", [("LaunchApp", 34 | 192, "INSTALLDIR", '"[INSTALLDIR]xpcode.exe"')])

    add_ui(db, version)
    db.Commit()
    shutil.rmtree(stage)
    print("wrote %s (%d bytes)" % (msi_path, os.path.getsize(msi_path)))


if __name__ == "__main__":
    main()
