#!/usr/bin/env python
import os, copy


def options(opt):
    opt.load('compiler_cxx')
    opt.load('boost')


def configure(cfg):
    cfg.check_waf_version(mini='1.6.10') # ECM: bleeding EDGE!!1! (needed for multiple boost checks below)
    cfg.load('compiler_cxx')
    cfg.load('boost')

    cfg.check_cfg(package='zlib', args='--libs --cflags')
    cfg.check_cxx(lib='pthread')
    cfg.check_boost(lib='system thread', uselib_store='BOOST4RCF')
    cfg.check_boost(lib='serialization system thread', uselib_store='BOOST4RCF_WSERIALIZATION')
    cfg.check_boost(lib='filesystem serialization system thread', uselib_store='BOOST4RCF_WSERIALIZATION_WFS')

    DEFINES_common = [
        'RCF_USE_BOOST_ASIO',
        'RCF_USE_BOOST_THREADS',
        'RCF_USE_ZLIB',
        'RCF_NO_AUTO_INIT_DEINIT',
    ]
    cfg.env.DEFINES_RCFSF      = DEFINES_common + [ 'RCF_USE_SF_SERIALIZATION' ]
    cfg.env.DEFINES_RCFBOOST   = DEFINES_common + [ 'RCF_USE_BOOST_SERIALIZATION' ]
    cfg.env.DEFINES_RCFBOOSTSF = DEFINES_common + [ 'RCF_USE_BOOST_SERIALIZATION', 'RCF_USE_SF_SERIALIZATION' ]


def build(bld):
    inc = bld.path.find_dir('include').abspath()
    common_flags = { "cxxflags"  : [],
              'linkflags' : [],
              'includes'  : [inc],
              'defines'   : [],
              'use'       : [ 'ZLIB', 'PTHREAD' ],
    }

    bld(
        target          = 'rcf_inc',
        export_includes = inc
    )

    # TODO: ugly target names, but for backwards compatibility
    for i,s in enumerate(['rcf', 'sf', 'rcfsf', 'rcf_fs']):
        flags = copy.deepcopy(common_flags)
        if s == 'sf': # pure sf
            flags['use'].append('BOOST4RCF')
            flags['use'].append('RCFSF')
        if s == 'rcf': # pure boost
            flags['use'].append('BOOST4RCF_WSERIALIZATION')
            flags['use'].append('RCFBOOST')
        if s == 'rcf_fs': # pure boost with filesystem support
            flags['use'].append('BOOST4RCF_WSERIALIZATION_WFS')
            flags['use'].append('RCFBOOST')
            flags['defines'] += ['RCF_USE_BOOST_FILESYSTEM', 'BOOST_FILESYSTEM_DEPRECATED']
        if s == 'rcfsf': # both
            flags['use'].append('BOOST4RCF_WSERIALIZATION')
            flags['use'].append('RCFBOOSTSF')

        bld(
                features        = 'cxx cxxshlib',
                target          = s,
                idx             = i,
                source          = 'src/RCF/RCF.cpp',
                export_includes = inc,
                install_path    = '${PREFIX}/lib',
                **flags
        )
