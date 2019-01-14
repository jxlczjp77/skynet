#include <windows.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>

void RedirectIOToConsole() {
	// allocate a console for this app
	if (!AllocConsole()) {
		return;
	}
	AttachConsole(GetCurrentProcessId());
	freopen("CON", "w", stdout);
	freopen("CON", "w", stderr);
	freopen("CON", "r", stdin);
	// MessageBoxA(0, 0, 0, 0);
}
