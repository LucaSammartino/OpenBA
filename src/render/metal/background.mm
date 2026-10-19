#include "app/options.h"
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <cstdlib>
extern "C" void OpenBAConfigureBackgroundWindow(SDL_Window* w) {
 if(!Opt().background)return;
 [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
 if(!w)return;
 SDL_SetWindowSize(w,Opt().windowWidth > 0 ? Opt().windowWidth : 640,Opt().windowHeight > 0 ? Opt().windowHeight : 360);SDL_SetWindowPosition(w,8,32);
 NSWindow* native=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(w),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
 [native setHidesOnDeactivate:NO];[native setLevel:NSNormalWindowLevel-1];[native orderBack:nil];
}
