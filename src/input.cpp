#include "input.hpp"
#include "SDL2/SDL.h"
#include <cassert>
#include <map>
#include <vector>
using namespace std;

namespace VTxx {

void InputDev::write(uint8_t addr, uint8_t data) { assert(false); }

uint8_t InputDev::read(uint8_t addr) {
  uint8_t res = shiftreg & 0x01;
  shiftreg >>= 1;
  shiftreg |= ((btn_state & 0x01) << 7);
  return res;
}
// Map keys to input bits
static const map<SDL_Scancode, int> keys = {
    {SDL_SCANCODE_X, 0},      {SDL_SCANCODE_Z, 1},    {SDL_SCANCODE_RSHIFT, 2},
    {SDL_SCANCODE_RETURN, 3}, {SDL_SCANCODE_UP, 4},   {SDL_SCANCODE_DOWN, 5},
    {SDL_SCANCODE_LEFT, 6},   {SDL_SCANCODE_RIGHT, 7}};

void InputDev::process_event(SDL_Event *ev) {
  switch (ev->type) {
  case SDL_KEYDOWN:
    if (keys.find(ev->key.keysym.scancode) != keys.end()) {
      btn_state |= (1 << keys.at(ev->key.keysym.scancode));
    }
    break;
  case SDL_KEYUP:
    if (keys.find(ev->key.keysym.scancode) != keys.end()) {
      btn_state &= ~(1 << keys.at(ev->key.keysym.scancode));
    }
    break;
  }
}

} // namespace VTxx