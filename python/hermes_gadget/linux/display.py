"""An optional SDL display for the device renderer, with keyboard and touch input."""

import os

from ..sim.native import BUTTONS


class Display:
    def __init__(self, config: dict):
        os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
        try:
            import pygame
        except ImportError as exc:
            raise RuntimeError("install the display extra or the system python3-pygame package") from exc
        self.pg = pygame
        self.width = config.get("width", 320)
        self.height = config.get("height", 240)
        self.rotation = config.get("rotation", 0)
        self.touch = config.get("touch", True)
        self.round = config.get("round", False)
        self.dirty = True
        self.held = False
        self.keys = set()
        self.last_frame = -100
        size = (self.height, self.width) if self.rotation in (90, 270) else (self.width, self.height)
        try:
            pygame.display.init()
            fullscreen = config.get("fullscreen", True)
            self.window = pygame.display.set_mode((0, 0) if fullscreen else size,
                                                  pygame.FULLSCREEN if fullscreen else pygame.RESIZABLE)
            pygame.display.set_caption("Hermes Gadget")
            pygame.mouse.set_visible(not fullscreen)
            self.frame = pygame.Surface((self.width, self.height), depth=16, masks=(0xF800, 0x07E0, 0x001F, 0))
            self.viewport = pygame.Rect((0, 0), size).fit(self.window.get_rect())
        except pygame.error as exc:
            pygame.display.quit()
            raise RuntimeError(f"display unavailable: {exc}; check SDL display access") from exc

    def coordinates(self, position: tuple[int, int]) -> tuple[int, int] | None:
        if not self.viewport.collidepoint(position):
            return None
        rw, rh = (self.height, self.width) if self.rotation in (90, 270) else (self.width, self.height)
        x = min(rw - 1, (position[0] - self.viewport.x) * rw // self.viewport.width)
        y = min(rh - 1, (position[1] - self.viewport.y) * rh // self.viewport.height)
        if self.rotation == 90:
            return self.width - 1 - y, x
        if self.rotation == 180:
            return self.width - 1 - x, self.height - 1 - y
        if self.rotation == 270:
            return y, self.height - 1 - x
        return x, y

    def poll(self, device) -> bool:
        pg = self.pg
        keys = {pg.K_SPACE: "talk", pg.K_ESCAPE: "cancel", pg.K_UP: "up", pg.K_DOWN: "down"}
        for event in pg.event.get():
            if event.type == pg.QUIT:
                return False
            if event.type == pg.WINDOWFOCUSLOST:
                for key in self.keys:
                    device.button(BUTTONS[keys[key]], False)
                self.keys.clear()
                if self.held:
                    device.touch(False)
                    self.held = False
            elif event.type in (pg.KEYDOWN, pg.KEYUP) and event.key in keys:
                pressed = event.type == pg.KEYDOWN
                if pressed != (event.key in self.keys):
                    if pressed:
                        self.keys.add(event.key)
                    else:
                        self.keys.discard(event.key)
                    device.button(BUTTONS[keys[event.key]], pressed)
            elif self.touch and event.type == pg.MOUSEBUTTONDOWN and event.button == 1:
                point = self.coordinates(event.pos)
                if point:
                    self.held = True
                    device.touch(True, *point)
            elif self.touch and event.type == pg.MOUSEMOTION and self.held:
                point = self.coordinates(event.pos)
                if point:
                    device.touch(True, *point)
            elif event.type == pg.MOUSEBUTTONUP and event.button == 1 and self.held:
                device.touch(False)
                self.held = False
            elif event.type in (pg.VIDEORESIZE, pg.WINDOWEXPOSED):
                self.dirty = True
        return True

    def present(self, device, now_ms: int) -> None:
        if not self.dirty or now_ms - self.last_frame < 33:
            return
        pg = self.pg
        pixels = device.framebuffer_rows()
        # SDL aligns each row to four bytes; supported widths are even.
        self.frame.get_buffer().write(pixels)
        frame = pg.transform.rotate(self.frame, self.rotation) if self.rotation else self.frame
        self.viewport = frame.get_rect().fit(self.window.get_rect())
        self.window.fill((0, 0, 0))
        self.window.blit(pg.transform.scale(frame, self.viewport.size), self.viewport)
        pg.display.flip()
        self.dirty = False
        self.last_frame = now_ms

    def close(self) -> None:
        self.pg.display.quit()
