# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize("target", ["esp32"], indirect=["target"])
def test_jrnl_bdl(dut: Dut) -> None:
    # reset=True is required: the power-off recovery cases are TEST_CASE_MULTIPLE_STAGES and
    # rely on a real device reset (esp_restart) between stages.
    dut.run_all_single_board_cases(reset=True)
