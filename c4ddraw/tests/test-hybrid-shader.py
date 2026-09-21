"""Render the shipping GLSL through WGL; no third-party Python dependencies.

Run on Windows with a hardware OpenGL driver: python test-hybrid-shader.py
The reference paths are the two existing shader files, rendered separately.
"""
import ctypes as C
from ctypes import wintypes as W
import math
from pathlib import Path
import random
import sys


def api(dll, name, restype, *argtypes):
    fn = getattr(dll, name)
    fn.restype, fn.argtypes = restype, argtypes
    return fn


class GL:
    def __init__(self):
        self.u = C.WinDLL('user32', use_last_error=True)
        self.g = C.WinDLL('gdi32', use_last_error=True)
        self.o = C.WinDLL('opengl32', use_last_error=True)
        create = api(self.u, 'CreateWindowExW', W.HWND, W.DWORD, W.LPCWSTR,
                     W.LPCWSTR, W.DWORD, C.c_int, C.c_int, C.c_int, C.c_int,
                     W.HWND, W.HMENU, W.HINSTANCE, C.c_void_p)
        self.hwnd = create(0, 'STATIC', '', 0x80000000, 0, 0, 128, 128,
                           None, None, None, None)
        assert self.hwnd, C.get_last_error()
        self.dc = api(self.u, 'GetDC', W.HDC, W.HWND)(self.hwnd)
        # PIXELFORMATDESCRIPTOR: RGBA, draw-to-window, OpenGL, 32-bit color.
        pfd = C.create_string_buffer(40)
        pfd[0:4] = bytes([40, 0, 1, 0])
        pfd[4:8] = (0x24).to_bytes(4, 'little')
        pfd[8:10] = bytes([0, 32])
        fmt = api(self.g, 'ChoosePixelFormat', C.c_int, W.HDC, C.c_void_p)(self.dc, pfd)
        assert fmt
        assert api(self.g, 'SetPixelFormat', W.BOOL, W.HDC, C.c_int, C.c_void_p)(self.dc, fmt, pfd)
        self.context = api(self.o, 'wglCreateContext', C.c_void_p, W.HDC)(self.dc)
        assert self.context
        self.make_current = api(self.o, 'wglMakeCurrent', W.BOOL, W.HDC, C.c_void_p)
        assert self.make_current(self.dc, self.context)
        self.proc = api(self.o, 'wglGetProcAddress', C.c_void_p, C.c_char_p)
        self.functions = {}
        self.bind('glGetString', C.c_char_p, C.c_uint)
        print('OpenGL:', self.glGetString(0x1F02).decode(),
              self.glGetString(0x1F01).decode())
        for name, result, args in [
            ('glCreateShader', C.c_uint, [C.c_uint]),
            ('glShaderSource', None, [C.c_uint, C.c_int, C.POINTER(C.c_char_p), C.c_void_p]),
            ('glCompileShader', None, [C.c_uint]),
            ('glGetShaderiv', None, [C.c_uint, C.c_uint, C.POINTER(C.c_int)]),
            ('glGetShaderInfoLog', None, [C.c_uint, C.c_int, C.c_void_p, C.c_void_p]),
            ('glCreateProgram', C.c_uint, []),
            ('glAttachShader', None, [C.c_uint, C.c_uint]),
            ('glLinkProgram', None, [C.c_uint]),
            ('glGetProgramiv', None, [C.c_uint, C.c_uint, C.POINTER(C.c_int)]),
            ('glGetProgramInfoLog', None, [C.c_uint, C.c_int, C.c_void_p, C.c_void_p]),
            ('glUseProgram', None, [C.c_uint]),
            ('glGetUniformLocation', C.c_int, [C.c_uint, C.c_char_p]),
            ('glUniform1i', None, [C.c_int, C.c_int]),
            ('glUniform2f', None, [C.c_int, C.c_float, C.c_float]),
            ('glUniformMatrix4fv', None, [C.c_int, C.c_int, C.c_ubyte, C.POINTER(C.c_float)]),
            ('glGetAttribLocation', C.c_int, [C.c_uint, C.c_char_p]),
            ('glEnableVertexAttribArray', None, [C.c_uint]),
            ('glVertexAttribPointer', None, [C.c_uint, C.c_int, C.c_uint, C.c_ubyte, C.c_int, C.c_void_p]),
            ('glGenTextures', None, [C.c_int, C.POINTER(C.c_uint)]),
            ('glBindTexture', None, [C.c_uint, C.c_uint]),
            ('glTexParameteri', None, [C.c_uint, C.c_uint, C.c_int]),
            ('glTexImage2D', None, [C.c_uint, C.c_int, C.c_int, C.c_int, C.c_int,
                                    C.c_int, C.c_uint, C.c_uint, C.c_void_p]),
            ('glGenFramebuffers', None, [C.c_int, C.POINTER(C.c_uint)]),
            ('glBindFramebuffer', None, [C.c_uint, C.c_uint]),
            ('glFramebufferTexture2D', None, [C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_int]),
            ('glCheckFramebufferStatus', C.c_uint, [C.c_uint]),
            ('glViewport', None, [C.c_int, C.c_int, C.c_int, C.c_int]),
            ('glDrawArrays', None, [C.c_uint, C.c_int, C.c_int]),
            ('glReadPixels', None, [C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_void_p]),
            ('glGetError', C.c_uint, []),
        ]:
            self.bind(name, result, *args)
        self.output = self.texture(128, 128, None, True)
        self.fbo = C.c_uint()
        self.glGenFramebuffers(1, C.byref(self.fbo))
        self.glBindFramebuffer(0x8D40, self.fbo)
        self.glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, self.output, 0)
        assert self.glCheckFramebufferStatus(0x8D40) == 0x8CD5

    def bind(self, name, restype, *args):
        try:
            fn = api(self.o, name, restype, *args)
        except AttributeError:
            address = self.proc(name.encode())
            assert address not in (None, 1, 2, 3, C.c_void_p(-1).value), name
            fn = C.WINFUNCTYPE(restype, *args)(address)
        setattr(self, name, fn)

    def program(self, source, version=120):
        program = self.glCreateProgram()
        for stage, enum in [('VERTEX', 0x8B31), ('FRAGMENT', 0x8B30)]:
            shader = self.glCreateShader(enum)
            text = ('#version %d\n#define %s\n' % (version, stage) + source).encode()
            code = C.c_char_p(text)
            self.glShaderSource(shader, 1, C.byref(code), None)
            self.glCompileShader(shader)
            status = C.c_int()
            self.glGetShaderiv(shader, 0x8B81, C.byref(status))
            log = C.create_string_buffer(8192)
            self.glGetShaderInfoLog(shader, len(log), None, log)
            assert status.value, stage + ': ' + log.value.decode()
            self.glAttachShader(program, shader)
        self.glLinkProgram(program)
        status = C.c_int()
        self.glGetProgramiv(program, 0x8B82, C.byref(status))
        log = C.create_string_buffer(8192)
        self.glGetProgramInfoLog(program, len(log), None, log)
        assert status.value, log.value.decode()
        return program

    def texture(self, w, h, pixels, floating=False):
        texture = C.c_uint()
        self.glGenTextures(1, C.byref(texture))
        self.glBindTexture(0x0DE1, texture)
        for prop in (0x2801, 0x2800):
            self.glTexParameteri(0x0DE1, prop, 0x2600)
        for prop in (0x2802, 0x2803):
            self.glTexParameteri(0x0DE1, prop, 0x812F)
        data = None if pixels is None else (C.c_float * len(pixels))(*pixels)
        self.glTexImage2D(0x0DE1, 0, 0x8814 if floating else 0x8058,
                          w, h, 0, 0x1908, 0x1406, data)
        return texture.value

    def render(self, program, pixels, iw, ih, tw, th, ow, oh, linear=False):
        self.glUseProgram(program)
        self.texture(tw, th, pixels)
        for prop in (0x2801, 0x2800):
            self.glTexParameteri(0x0DE1, prop, 0x2601 if linear else 0x2600)
        for name in (b'Texture', b's_p'):
            self.glUniform1i(self.glGetUniformLocation(program, name), 0)
        for name, x, y in [(b'TextureSize', tw, th), (b'InputSize', iw, ih), (b'OutputSize', ow, oh)]:
            self.glUniform2f(self.glGetUniformLocation(program, name), x, y)
        identity = (C.c_float * 16)(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        self.glUniformMatrix4fv(self.glGetUniformLocation(program, b'MVPMatrix'), 1, False, identity)
        position = (C.c_float * 8)(-1, -1, 1, -1, -1, 1, 1, 1)
        texcoord = (C.c_float * 8)(0, 0, iw / tw, 0, 0, ih / th, iw / tw, ih / th)
        for name, array in [(b'VertexCoord', position), (b'TexCoord', texcoord)]:
            location = self.glGetAttribLocation(program, name)
            assert location >= 0
            self.glEnableVertexAttribArray(location)
            self.glVertexAttribPointer(location, 2, 0x1406, False, 0, array)
        self.glViewport(0, 0, ow, oh)
        self.glDrawArrays(0x0005, 0, 4)
        result = (C.c_float * (ow * oh * 4))()
        self.glReadPixels(0, 0, ow, oh, 0x1908, 0x1406, result)
        assert self.glGetError() == 0
        return list(result)

    def close(self):
        self.make_current(None, None)
        api(self.o, 'wglDeleteContext', W.BOOL, C.c_void_p)(self.context)
        api(self.u, 'ReleaseDC', C.c_int, W.HWND, W.HDC)(self.hwnd, self.dc)
        api(self.u, 'DestroyWindow', W.BOOL, W.HWND)(self.hwnd)


def rgb(values):
    return [v for i, v in enumerate(values) if i % 4 != 3]


def error(a, b):
    assert all(math.isfinite(v) for v in a + b)
    return max(abs(x - y) for x, y in zip(a, b))


def main():
    shader_dir = Path(__file__).resolve().parents[1] / 'release/Shaders/interpolation'
    sources = [(shader_dir / name).read_text() for name in
               ['lanczos-bicubic.glsl', 'lanczos2-sharp.glsl', 'catmull-rom-bilinear.glsl']]
    gl = GL()
    try:
        hybrid, lanczos, bicubic = [gl.program(source) for source in sources]
        gl.program(sources[0], 130)
        rng = random.Random(220)
        count = 2  # GLSL 120 and 130 compilation/linking.
        max_midpoint = 0.0
        for iw, ih, ow, oh in [(8, 8, 8, 8), (8, 8, 16, 16), (9, 7, 23, 19), (16, 12, 7, 5)]:
            pixels = [rng.randrange(256) / 255 for _ in range(iw * ih * 4)]
            result = gl.render(hybrid, pixels, iw, ih, iw, ih, ow, oh)
            left = gl.render(lanczos, pixels, iw, ih, iw, ih, ow, oh)
            right = gl.render(bicubic, pixels, iw, ih, iw, ih, ow, oh, True)
            midpoint = [(a + b) / 2 for a, b in zip(rgb(left), rgb(right))]
            delta = error(rgb(result), midpoint)
            max_midpoint = max(max_midpoint, delta)
            # Hardware bilinear interpolation has finite subtexel precision.
            assert delta < 0.003, (iw, ih, ow, oh, delta)
            assert all(v == 1 for v in result[3::4])
            count += 2
        for iw, ih, tw, th in [(1, 1, 8, 8), (7, 5, 16, 8), (11, 9, 16, 16)]:
            # Poison unused POT/crop padding. Active pixels must give exactly
            # the same result with or without it, including every boundary.
            active = [[rng.randrange(256) / 255 for _ in range(4)] for _ in range(iw * ih)]
            packed = [v for pixel in active for v in pixel]
            padded = []
            for y in range(th):
                for x in range(tw):
                    padded.extend(active[y * iw + x] if x < iw and y < ih else [1, 0, 1, 0])
            a = gl.render(hybrid, packed, iw, ih, iw, ih, 31, 27)
            b = gl.render(hybrid, padded, iw, ih, tw, th, 31, 27)
            assert error(a, b) < 0.0001, (iw, ih, error(a, b))
            count += 1
        for color in [(0, 0, 0, 0), (1, 1, 1, 0), (51 / 255, 102 / 255, 153 / 255, 0)]:
            a = gl.render(hybrid, list(color) * 35, 7, 5, 7, 5, 29, 23)
            assert error(a, list(color[:3] + (1,)) * (29 * 23)) < 0.0001
            count += 1
        print('%d checks passed; max midpoint difference %.8f' % (count, max_midpoint))
    finally:
        gl.close()


if __name__ == '__main__':
    if sys.platform != 'win32':
        raise SystemExit('This real-GL test requires Windows and an OpenGL driver.')
    main()
