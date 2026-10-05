"""Exercise the actual native first-run chooser using isolated registry keys."""
import argparse
import json
import pathlib
import subprocess
import time
import uuid
import winreg

from pywinauto import Desktop, keyboard
from pywinauto.controls.hwndwrapper import HwndWrapper


def read_language(key):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as handle:
            value, kind = winreg.QueryValueEx(handle, "Language")
            return value, kind
    except FileNotFoundError:
        return None


def wait_window(process):
    until = time.monotonic() + 15
    while time.monotonic() < until:
        windows = Desktop(backend="uia").windows(process=process.pid, class_name="VetroLook.LanguageOnboarding")
        if windows:
            return windows[0]
        if process.poll() is not None:
            raise AssertionError(f"Chooser exited early: {process.returncode}")
        time.sleep(.1)
    raise AssertionError("Chooser did not appear")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    exe = str(pathlib.Path(args.exe).resolve())
    original = read_language(r"Software\VetroLook\Settings")
    checks = []
    for case, choice, invalid in [("english", 1, False), ("russian", 0, False), ("invalid-setting", 0, True)]:
        key = "Software\\VetroLook\\OnboardingTests\\" + str(uuid.uuid4())
        report = output / f"{case}.txt"
        process = None
        try:
            if invalid:
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key) as handle:
                    winreg.SetValueEx(handle, "Language", 0, winreg.REG_DWORD, 7)
            command = [exe, "--language-onboarding-test", key, str(report)]
            process = subprocess.Popen(command)
            window = wait_window(process)
            native = HwndWrapper(window.handle)
            native.set_focus()
            time.sleep(.3)
            assert native.window_text() == "VetroLook — Choose your language", ascii(native.window_text())
            window.capture_as_image().save(output / f"{case}.png")
            native.post_message(0x0010)  # WM_CLOSE
            native.post_message(0x0112, 0xF060)  # SC_CLOSE
            keyboard.send_keys("{ESC}%{F4}")
            time.sleep(.35)
            assert process.poll() is None, "Close or Escape dismissed the question"
            assert read_language(key) == ((7, winreg.REG_DWORD) if invalid else None)
            checks.append(case + ": close, Alt+F4 and Escape blocked without saving")
            native.set_focus()
            if choice == 1:
                keyboard.send_keys("{TAB}{ENTER}")
            else:
                rect = window.rectangle()
                window.click_input(coords=(int(rect.width() * .72), int(rect.height() * .805)))
            assert process.wait(timeout=10) == 0
            assert read_language(key) == (choice, winreg.REG_DWORD)
            assert f"language={choice}" in report.read_text()
            checks.append(case + ": explicit choice persisted")
            subprocess.run(command, check=True, timeout=10)
            assert "prompt_shown=0" in report.read_text()
            checks.append(case + ": restart skips the question")
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
            except FileNotFoundError:
                pass
    assert read_language(r"Software\VetroLook\Settings") == original
    result = {"all_passed": True, "production_language_unchanged": True, "checks": checks}
    (output / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
