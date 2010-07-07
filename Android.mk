#
# Copyright (C) 2009 The Android Open Source Project
# Copyright (C) 2010, Code Aurora Forum. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

common_msm_dirs := liblights librpc libgralloc-qsd8k
msm7k_dirs := $(common_msm_dirs) boot libaudio libcopybit
qsd8k_dirs := $(common_msm_dirs) libaudio-qsd8k dspcrashd libcopybit
msm7x30_dirs := $(common_msm_dirs) libaudio
msm8660_dirs := $(common_msm_dirs)

ifeq ($(TARGET_BOARD_PLATFORM),msm7k)
  ifeq "$(findstring msm7630,$(TARGET_PRODUCT))" "msm7630"
    include $(call all-named-subdir-makefiles,$(msm7x30_dirs))
  else
    include $(call all-named-subdir-makefiles,$(msm7k_dirs))
  endif
else
  ifeq ($(TARGET_BOARD_PLATFORM),qsd8k)
    include $(call all-named-subdir-makefiles,$(qsd8k_dirs))
  else
    ifeq ($(TARGET_BOARD_PLATFORM),msm8660)
      include $(call all-named-subdir-makefiles,$(msm8660_dirs))
    endif
  endif
endif
