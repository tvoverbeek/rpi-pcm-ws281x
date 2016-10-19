#!/usr/bin/env python
# Python wrapper for the rpi_pcm_ws281x library.
# Author: Tony DiCola (tony@tonydicola.com)
from setuptools import setup, find_packages, Extension
from setuptools.command.build_py import build_py
import subprocess

class CustomInstallCommand(build_py):
    """Customized install to run library Makefile"""
    def run(self):
        print("Compiling ws281x library...")
        subprocess.Popen(["make","-Clib","lib"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        build_py.run(self)

setup(name              = 'rpi_pcm_ws281x',
      version           = '1.0.0',
      author            = 'Ton van Overbeek',
      author_email      = 'tvoverbeek@gmail.com',
      description       = 'Userspace Raspberry Pi PCM library for WS281X LEDs.',
      license           = 'MIT',
      url               = 'https://github.com/tvoverbeek/rpi_pcm_ws281x/',
      cmdclass          = {'build_py':CustomInstallCommand},
      py_modules        = ['neopixel_pcm'],
      ext_modules       = [Extension('_rpi_pcm_ws281x', 
                                     sources=['rpi_pcm_ws281x_wrap.c'],
                                     include_dirs=['lib/'],
                                     library_dirs=['lib/'],
                                     libraries=['ws2811-pcm'])])
