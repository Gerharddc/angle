# Copyright (c) 2013-2014 The ANGLE Project Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script generates visual studio projects that can be used on WinRT

import os
import sys

script_dir = os.path.join(os.path.dirname(__file__), 'build')
angle_dir = os.path.normpath(os.path.join(script_dir, os.pardir))
gyp_dir = os.path.join(angle_dir, 'third_party', 'gyp')

gyp_generators = "msvs"

def generateProjects(generation_dir, build_winphone, msvs_version, app_type_rev,
                     target_platform_ver):
    gyp_cmd = os.path.join(gyp_dir, 'gyp')
    gyp_cmd += ' --ignore-environment'
    gyp_cmd += ' --depth=.'
    gyp_cmd += ' --include=' + os.path.join(script_dir, 'common.gypi')
    gyp_cmd += ' --generator-output=' + generation_dir
    gyp_cmd += ' --format=' + gyp_generators
    gyp_cmd += ' -G msvs_version=' + msvs_version
    gyp_cmd += ' -D angle_use_commit_id=1'
    gyp_cmd += ' -D angle_build_winrt=1'
    gyp_cmd += ' -D angle_build_winrt_app_type_revision=' + app_type_rev
    gyp_cmd += ' -D angle_build_winrt_target_platform_ver=' + target_platform_ver
    gyp_cmd += ' -D angle_build_winphone=' + ('1' if build_winphone else '0')
    gyp_cmd += ' -D angle_enable_d3d9=0'
    gyp_cmd += ' -D angle_enable_gl=0'
    gyp_cmd += ' -D angle_standalone=1'
    gyp_cmd += ' ' + os.path.join(script_dir, 'angle.gyp')

    print 'Generating projects to ' + generation_dir + ' from gyp files...'
    print gyp_cmd
    sys.stdout.flush()
    os.system(gyp_cmd)

if __name__ == '__main__':
    # Generate Windows 8.1 projects
    generateProjects("winrt/8.1/windows", False, "2013e", "8.1", "");
    generateProjects("winrt/8.1/windowsphone", True, "2013e", "8.1", "");

    # Generate Windows 10 projects
    generateProjects("winrt/10", False, "2015", "10.0", "10.0.10240.0");
