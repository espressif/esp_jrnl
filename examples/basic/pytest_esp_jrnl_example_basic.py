# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded import Dut

@pytest.mark.generic
def test_jrnl_example_basic(dut: Dut) -> None:
    dut.expect_exact('Journaled FatFS mounted successfully.')
    dut.expect_exact('Opening file')
    dut.expect_exact('File written')
    dut.expect_exact('Renaming file')
    dut.expect_exact('Reading file')
    dut.expect_exact('Read from file: \'Hello World!\'')
    dut.expect_exact('Journaled FatFS unmounted.')
