#include "w25q64.h"
#include "boot_qspi.h"

#define QSPI_TIMEOUT_MS 200U

static int cmd_no_data(uint8_t opcode)
{
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = opcode;
    cmd.AddressMode       = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_NONE;
    cmd.DummyCycles       = 0;
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    return HAL_QSPI_Command(&hqspi, &cmd, QSPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int cmd_read_reg(uint8_t opcode, uint8_t *value)
{
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = opcode;
    cmd.AddressMode       = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_1_LINE;
    cmd.DummyCycles       = 0;
    cmd.NbData            = 1;
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, QSPI_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }
    return HAL_QSPI_Receive(&hqspi, value, QSPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int cmd_write_reg(uint8_t opcode, uint8_t value)
{
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = opcode;
    cmd.AddressMode       = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_1_LINE;
    cmd.DummyCycles       = 0;
    cmd.NbData            = 1;
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, QSPI_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }
    return HAL_QSPI_Transmit(&hqspi, &value, QSPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int wait_not_busy(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        uint8_t sr1 = 0;
        if (cmd_read_reg(W25Q_CMD_READ_SR1, &sr1) != 0)
        {
            return -1;
        }
        if ((sr1 & W25Q_SR1_BUSY) == 0)
        {
            return 0;
        }
    }
    return -2;
}

uint32_t w25q64_read_jedec_id(void)
{
    QSPI_CommandTypeDef cmd = {0};
    uint8_t id[3] = {0};

    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = W25Q_CMD_JEDEC_ID;
    cmd.AddressMode       = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_1_LINE;
    cmd.DummyCycles       = 0;
    cmd.NbData            = 3;
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, QSPI_TIMEOUT_MS) != HAL_OK)
    {
        return 0;
    }
    if (HAL_QSPI_Receive(&hqspi, id, QSPI_TIMEOUT_MS) != HAL_OK)
    {
        return 0;
    }
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
}

int w25q64_reset(void)
{
    if (cmd_no_data(W25Q_CMD_ENABLE_RESET) != 0)
    {
        return -1;
    }
    if (cmd_no_data(W25Q_CMD_RESET) != 0)
    {
        return -2;
    }
    /* W25Q datasheet: tRST_R = 30us; HAL_Delay only has 1ms granularity. */
    HAL_Delay(1);
    return 0;
}

int w25q64_enable_qe(void)
{
    uint8_t sr2 = 0;
    if (cmd_read_reg(W25Q_CMD_READ_SR2, &sr2) != 0)
    {
        return -1;
    }
    if ((sr2 & W25Q_SR2_QE) != 0)
    {
        return 0; /* Already set, common on JV parts shipped from factory. */
    }

    if (cmd_no_data(W25Q_CMD_WRITE_ENABLE) != 0)
    {
        return -2;
    }
    if (cmd_write_reg(W25Q_CMD_WRITE_SR2, (uint8_t)(sr2 | W25Q_SR2_QE)) != 0)
    {
        return -3;
    }
    return wait_not_busy(100);
}

int w25q64_enter_memory_mapped(void)
{
    QSPI_CommandTypeDef cmd = {0};
    QSPI_MemoryMappedTypeDef mm = {0};

    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = W25Q_CMD_FAST_READ_QUAD_IO;
    cmd.AddressMode       = QSPI_ADDRESS_4_LINES;
    cmd.AddressSize       = QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_4_LINES;
    cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    cmd.AlternateBytes    = 0xF0;       /* M5:0 = anything but 0xA0; keeps continuous-read mode off */
    cmd.DataMode          = QSPI_DATA_4_LINES;
    cmd.DummyCycles       = 4;          /* 0xEB needs 6 total dummy clocks of mode+dummy; M-byte consumes 2 */
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    mm.TimeOutPeriod     = 0;
    mm.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;

    return HAL_QSPI_MemoryMapped(&hqspi, &cmd, &mm) == HAL_OK ? 0 : -1;
}
