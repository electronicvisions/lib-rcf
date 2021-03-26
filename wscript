#!/usr/bin/env python
import os, copy

def depends(ctx):
    ctx('logger')
    ctx('hate')


def options(opt):
    opt.load('compiler_cxx')
    opt.load('boost')
    opt.load('shelltest')

    hopts = opt.add_option_group('RCF Options')
    hopts.add_option(
            '--rcf-enable-warnings',
            action='store_true', default=False,
            help='RCF produces a lot of warnings that are disabled by default.')

    hopts.add_option("--rcf-extensions-loglevel",
                     choices=["trace", "debug", "info", "warning", "error", "fatal"],
                     default="info",
                     help="Maximal loglevel to compile in rcf-extensions with.")


def configure(cfg):
    cfg.check_waf_version(mini='1.6.10') # ECM: bleeding EDGE!!1! (needed for multiple boost checks below)
    cfg.load('compiler_cxx')
    cfg.load('boost')

    cfg.check_cxx(lib="pthread", uselib_store="PTHREAD")

    cfg.env.rcf_enable_warnings = getattr(
            cfg.options, "rcf_enable_warnings", False)

    cfg.check_cfg(package='zlib', args='--libs --cflags')
    cfg.check_boost(lib='system', uselib_store='BOOST4RCF')
    cfg.check_boost(lib='serialization system', uselib_store='BOOST4RCF_WSERIALIZATION')
    cfg.check_boost(lib='filesystem serialization system', uselib_store='BOOST4RCF_WSERIALIZATION_WFS')

    cfg.check_boost(lib='program_options', uselib_store='BOOST_PO')

    cfg.env.DEFINES_RCF_COMMON     = ['RCF_USE_ZLIB']
    cfg.env.DEFINES_RCF_SF_ONLY    = ['RCF_USE_SF_SERIALIZATION'] # automatically defined
    cfg.env.DEFINES_RCF_BOOST_ONLY = ['RCF_USE_BOOST_SERIALIZATION']
    cfg.env.DEFINES_RCF_SF_BOOST   = ['RCF_USE_BOOST_SERIALIZATION',
                                      'RCF_USE_SF_SERIALIZATION']
    cfg.env.DEFINES_RCF_BOOST_FS   = ['RCF_USE_BOOST_FILESYSTEM',
                                      'BOOST_FILESYSTEM_DEPRECATED']

    cfg.check_cxx(lib="dl", uselib_store="DL4RCF", mandatory=True)
    cfg.check_cxx(lib="uuid", uselib_store="UUID4RCF", mandatory=True)

    inc = cfg.path.find_dir('rcf-core/include').abspath()
    if getattr(cfg.options, "rcf_enable_warnings", False):
        cfg.check(msg="Enabling RCF warnings",
                  features='cxx',
                  includes=[inc],
                  uselib_store="rcf_includes")
    else:
        # suppress warnings by declaring include directory as system header
        cfg.check(msg="Checking if RCF warnings can be suppressed",
                  features='cxx',
                  cxxflags=['-isystem', inc],
                  uselib_store="rcf_includes")

    cfg.env.INCLUDES_RCF_EXTENSIONS = cfg.path.find_dir('rcf-extensions/include').abspath()

    cfg.define(
        "RCF_LOG_THRESHOLD",
        {'trace':   0,
         'debug':   1,
         'info':    2,
         'warning': 3,
         'error':   4,
         'fatal':   5}[cfg.options.rcf_extensions_loglevel]
    )

    cfg.load('shelltest')


def build(bld):
    if bld.env.rcf_enable_warnings:
        cxxflags = []
    else:
        cxxflags = ["-w"]

    common_flags = {
              'linkflags' : [],
              'includes'  : [],
              'cxxflags'  : cxxflags,
              'defines'   : [],
              'use'       : [
                  'rcf_includes',
                  'RCF_COMMON',
                  'PTHREAD',
                  'ZLIB',
                  'DL4RCF',
                  'UUID4RCF',
                  ],
    }

    # TODO: ugly target names, but for backwards compatibility
    # TODO: fix rcf-boost-fs target if it is needed in the future --obreitwi, 23-02-18 14:37:25
    rcf_targets = ['rcf-sf-only', 'rcf-boost-only', 'rcf-sf-boost']
    for i,s in enumerate(rcf_targets):
        flags = copy.deepcopy(common_flags)
        if s == 'rcf-sf-only': # pure sf
            flags['use'].extend([
                'BOOST4RCF',
                'RCF_SF_ONLY'
                ])
        if s == 'rcf-boost-only': # pure boost
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION',
                'RCF_BOOST_ONLY'
                ])
        if s == 'rcf-boost-fs': # pure boost with filesystem support
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION_WFS',
                'RCF_BOOST_ONLY'
                ])
            flags['use'].append('BOOST4RCF_WSERIALIZATION_WFS')
        if s == 'rcf-sf-boost': # both
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION',
                'RCF_SF_BOOST'
                ])

        objects_flags = copy.deepcopy(flags)
        objects_flags['cxxflags'].append('-fPIC')
        programme_flags = copy.deepcopy(flags)
        programme_flags['use'].extend(['{}_objects'.format(s)])
        bld.objects(
                features        = 'cxx',
                target          = '{}_objects'.format(s),
                source          = 'rcf-core/src/RCF/RCF.cpp',
                install_path    = '${PREFIX}/lib',
                **objects_flags
        )

        bld(
                features        = 'cxx cxxshlib',
                target          = s,
                install_path    = '${PREFIX}/lib',
                **programme_flags
        )
    bld(
        target = "rcf_extensions",
        export_includes = bld.env.INCLUDES_RCF_EXTENSIONS,
        use = ["logger_obj", "hate_inc"])

    bld.recurse("playground/round-robin-scheduler")
    bld.recurse("playground/on-demand")
