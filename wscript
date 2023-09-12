# -*- Mode: python; py-indent-offset: 4; indent-tabs-mode: nil; coding: utf-8; -*-

import os
from waflib import Utils

VERSION = '0.1'
APPNAME = 'ndn-traffic-generator'

def options(opt):
    opt.load(['compiler_cxx', 'gnu_dirs'])
    opt.load(['default-compiler-flags', 'boost'],
             tooldir=['.waf-tools'])

def configure(conf):
    conf.load(['compiler_cxx', 'gnu_dirs',
               'default-compiler-flags', 'boost'])

    # Prefer pkgconf if it's installed, because it gives more correct results
    # on Fedora/CentOS/RHEL/etc. See https://bugzilla.redhat.com/show_bug.cgi?id=1953348
    # Store the result in env.PKGCONFIG, which is the variable used inside check_cfg()
    conf.find_program(['pkgconf', 'pkg-config'], var='PKGCONFIG')

    pkg_config_path = os.environ.get('PKG_CONFIG_PATH', f'{conf.env.LIBDIR}/pkgconfig')
    conf.check_cfg(package='libndn-cxx', args=['libndn-cxx >= 0.8.1', '--cflags', '--libs'],
                   uselib_store='NDN_CXX', pkg_config_path=pkg_config_path)

    boost_libs = ['date_time', 'filesystem', 'program_options', 'thread']
    conf.check_boost(lib=boost_libs, mt=True)

    conf.check_compiler_flags()

def build(bld):
    bld.program(target='ndn-traffic-client',
                source='src/ndn-traffic-client.cpp',
                use='NDN_CXX BOOST')

    bld.program(target='ndn-traffic-server',
                source='src/ndn-traffic-server.cpp',
                use='NDN_CXX BOOST')

    bld.install_files('${SYSCONFDIR}/ndn', ['ndn-traffic-client.conf.sample',
                                            'ndn-traffic-server.conf.sample'])

    if Utils.unversioned_sys_platform() == 'linux':
        systemd_units = bld.path.ant_glob('systemd/*.in')
        bld(features='subst',
            name='systemd-units',
            source=systemd_units,
            target=[u.change_ext('') for u in systemd_units])

def dist(ctx):
    ctx.algo = 'tar.xz'

def distcheck(ctx):
    ctx.algo = 'tar.xz'
