foreach s: [16, 24, 32, 48, 64, 128, 256, 512]
  s = '@0@x@0@'.format(s.to_string())
  install_data('leaf-hu-emulator_@0@.png'.format(s),
               rename: 'leaf-hu-emulator.png',
               install_dir: qemu_icondir / 'hicolor' / s / 'apps')
endforeach

install_data('leaf-hu-emulator_32x32.bmp',
             rename: 'leaf-hu-emulator.bmp',
             install_dir: qemu_icondir / 'hicolor' / '32x32' / 'apps')

install_data('qemu.svg',
             install_dir: qemu_icondir / 'hicolor' / 'scalable' / 'apps')
