#!/bin/bash
#
# Copyright (C) 2016 The CyanogenMod Project
# Copyright (C) 2017-2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

if [ -z "${DEVICE_COMMON}" ]; then
    echo ""
    echo "error: This is a script in a common tree. Please execute" $(basename $0) "from a device tree."
    echo ""
    exit 1
fi

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

function blob_fixup() {
    case "${1}" in
        vendor/bin/thermal-engine)
            sed -i 's|/system/etc|/vendor/etc|g' "${2}"
            ;;
        vendor/lib/libmmcamera2_sensor_modules.so)
            sed -i 's|system/etc|vendor/etc|g;
                    s|/system/lib|/vendor/lib|g;
                    s|/system/cameradata|/vendor/cameradata|g' "${2}"
            ;;
        vendor/lib/libril-qcril-hook-oem.so)
            "${PATCHELF}" --replace-needed libril.so libril_samsung.so "${2}"
            ;;
        # bionic no longer has LD_SHIM_LIBS: link the shims in directly
        vendor/lib/hw/camera.vendor.msm8974.so)
            "${PATCHELF_0_17_2}" --add-needed libshim_camera.so "${2}"
            # target SDK 22: libmmjpeg.so (dlopened for JPEG encoding) has text relocations
            "${PATCHELF_0_17_2}" --add-needed libshim_sdk22.so "${2}"
            ;;
        vendor/lib/libperipheral_client.so)
            "${PATCHELF_0_17_2}" --add-needed libshim_binder.so "${2}"
            ;;
        # target SDK 22: libmmcamera_faceproc.so has text relocations
        vendor/bin/mm-qcamera-daemon)
            "${PATCHELF_0_17_2}" --add-needed libshim_sdk22.so "${2}"
            ;;
    esac
}

setup_vendor "${DEVICE_COMMON}" "${VENDOR}" "${ANDROID_ROOT}" true

for BLOB_LIST in "${MY_DIR}"/common-proprietary-files*.txt; do
    extract "${BLOB_LIST}" "${SRC}"
done

export BOARD_COMMON=msm8974-common

"./../../${VENDOR}/${BOARD_COMMON}/extract-files.sh" "$@"
