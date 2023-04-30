# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import re

import pytest
from pytest_embedded import Dut

@pytest.mark.supported_targets
@pytest.mark.generic
def test_jrnl_basic(dut: Dut) -> None:
    dut.run_all_single_board_cases(reset=True)
