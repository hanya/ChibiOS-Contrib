/*
    ChibiOS/HAL - Copyright (C) 2016 Uladzimir Pylinsky aka barthess

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    lib_scsi.c
 * @brief   SCSI target driver source code.
 *
 * @addtogroup SCSI
 * @{
 */

#include <string.h>

#include "hal.h"
//#include "chprintf.h"

#include "lib_scsi.h"

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

typedef struct {
  uint32_t first_lba;
  uint16_t blk_cnt;
} data_request_t;

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Byte swapping function.
 *
 * @notapi
 */
static uint32_t swap_uint32(uint32_t val) {
  val =  ((val << 8)  & 0xFF00FF00 ) | ((val >> 8)  & 0x00FF00FF);
  return ((val << 16) & 0xFFFF0000)  | ((val >> 16) & 0x0000FFFF);
}

/**
 * @brief   Byte swapping function.
 *
 * @notapi
 */
static uint16_t swap_uint16(uint16_t val) {
  return ((val >> 8) & 0xff) | ((val & 0xff) << 8);
}

/**
 * @brief   Combines data request from byte array.
 *
 * @notapi
 */
static data_request_t decode_data_request(const uint8_t *cmd) {

  data_request_t req;
  uint32_t lba;
  uint16_t blk;

  memcpy(&lba, &cmd[2], sizeof(lba));
  memcpy(&blk, &cmd[7], sizeof(blk));

  req.first_lba = swap_uint32(lba);
  req.blk_cnt = swap_uint16(blk);

  return req;
}

/**
 * @brief   Fills sense structure.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] key     SCSI sense key
 * @param[in] code    SCSI sense code
 * @param[in] qual    SCSI sense qualifier
 *
 * @notapi
 */
static void set_sense(SCSITarget *scsip, uint8_t key,
                      uint8_t code, uint8_t qual) {

  scsi_sense_response_t *sense = &scsip->sense;
  memset(sense, 0 , sizeof(scsi_sense_response_t));

  sense->byte[0]  = 0x70;
  sense->byte[2]  = key;
  sense->byte[7]  = 8;
  sense->byte[12] = code;
  sense->byte[13] = qual;
}

/**
 * @brief   Sets all values in sense data to 'success' condition.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 *
 * @notapi
 */
static void set_sense_ok(SCSITarget *scsip) {
  set_sense(scsip, SCSI_SENSE_KEY_GOOD,
                   SCSI_ASENSE_NO_ADDITIONAL_INFORMATION,
                   SCSI_ASENSEQ_NO_QUALIFIER);
}

/**
 * @brief   Transmits data via transport channel.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] data    pointer to data buffer
 * @param[in] len     number of bytes to be transmitted
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool transmit_data(SCSITarget *scsip, const uint8_t *data, uint32_t len) {

  const SCSITransport *trp = scsip->config->transport;
  const uint32_t residue = len - trp->transmit(trp, data, len);

  if (residue > 0) {
    scsip->residue = residue;
    return SCSI_FAILED;
  }
  else {
    return SCSI_SUCCESS;
  }
}

/**
 * @brief   Stub for unhandled SCSI commands.
 * @details Sets error flags in sense data structure and returns error error.
 */
static bool cmd_unhandled(SCSITarget *scsip, const uint8_t *cmd) {
  (void)cmd;

  set_sense(scsip, SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                   SCSI_ASENSE_INVALID_COMMAND,
                   SCSI_ASENSEQ_NO_QUALIFIER);
  return SCSI_FAILED;
}

/**
 * @brief   Stub for unrealized but required SCSI commands.
 * @details Sets sense data in 'all OK' condition and returns success status.
 */
static bool cmd_ignored(SCSITarget *scsip, const uint8_t *cmd) {
  (void)scsip;
  (void)cmd;

  set_sense_ok(scsip);
  return SCSI_SUCCESS;
}

/**
 * @brief   SCSI inquiry command handler.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool inquiry(SCSITarget *scsip, const uint8_t *cmd) {

  if ((cmd[1] & 0b11) || cmd[2] != 0) {
    set_sense(scsip, SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                     SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                     SCSI_ASENSEQ_NO_QUALIFIER);
    return SCSI_FAILED;
  }
  else {
    return transmit_data(scsip, (const uint8_t *)scsip->config->inquiry_response,
                                sizeof(scsi_inquiry_response_t));
  }
}

/**
 * @brief   SCSI request sense command handler.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool request_sense(SCSITarget *scsip, const uint8_t *cmd) {

  uint32_t tmp;
  memcpy(&tmp, &cmd[1], 3);

  if ((tmp != 0) || (cmd[4] != sizeof(scsi_sense_response_t))) {
    set_sense(scsip, SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                     SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                     SCSI_ASENSEQ_NO_QUALIFIER);
    return SCSI_FAILED;
  }
  else {
    return transmit_data(scsip, (uint8_t *)&scsip->sense,
                                 sizeof(scsi_sense_response_t));
  }
}

/**
 * @brief   SCSI mode sense (6) command handler.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool mode_sense6(SCSITarget *scsip, const uint8_t *cmd) {
  (void)cmd;

  scsip->mode_sense.byte[0] = sizeof(scsi_mode_sense6_response_t) - 1;
  scsip->mode_sense.byte[1] = 0;
  if (blkIsWriteProtected(scsip->config->blkdev)) {
    scsip->mode_sense.byte[2] = 0x01 << 7;
  }
  else {
    scsip->mode_sense.byte[2] = 0;
  }
  scsip->mode_sense.byte[3] = 0;

  return transmit_data(scsip, (uint8_t *)&scsip->mode_sense,
                              sizeof(scsi_mode_sense6_response_t));
}

/**
 * @brief   SCSI read capacity (10) command handler.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool read_capacity10(SCSITarget *scsip, const uint8_t *cmd) {

  (void)cmd;

  BlockDeviceInfo bdi;
  blkGetInfo(scsip->config->blkdev, &bdi);
  scsi_read_capacity10_response_t ret;
  ret.block_size      = swap_uint32(bdi.blk_size);
  ret.last_block_addr = swap_uint32(bdi.blk_num - 1);

  return transmit_data(scsip, (uint8_t *)&ret,
                        sizeof(scsi_read_capacity10_response_t));
}

/**
 * @brief   Checks data request for media overflow.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 * @retval true       When media overflow detected.
 * @retval false      Otherwise.
 *
 * @notapi
 */
static bool data_overflow(SCSITarget *scsip, const data_request_t *req) {

  BlockDeviceInfo bdi;
  blkGetInfo(scsip->config->blkdev, &bdi);

  if (req->first_lba + req->blk_cnt > bdi.blk_num) {
    set_sense(scsip, SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                     SCSI_ASENSE_LBA_OUT_OF_RANGE,
                     SCSI_ASENSEQ_NO_QUALIFIER);
    return true;
  }
  else {
    return false;
  }
}

/**
 * @brief   SCSI read/write (10) command handler.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @notapi
 */
static bool data_read_write10(SCSITarget *scsip, const uint8_t *cmd) {

  data_request_t req = decode_data_request(cmd);

  if (data_overflow(scsip, &req)) {
    return SCSI_FAILED;
  }
  else {
    const SCSITransport *tr = scsip->config->transport;
    BaseBlockDevice *blkdev = scsip->config->blkdev;
    BlockDeviceInfo bdi;
    blkGetInfo(blkdev, &bdi);
    size_t bs = bdi.blk_size;
    uint8_t *buf = scsip->config->blkbuf;

    for (size_t i=0; i<req.blk_cnt; i++) {
      if (cmd[0] == SCSI_CMD_READ_10) {
        // TODO: block error handling
        blkRead(blkdev, req.first_lba + i, buf, 1);
        tr->transmit(tr, buf, bs);
      }
      else {
        // TODO: block error handling
        tr->receive(tr, buf, bs);
        blkWrite(blkdev, req.first_lba + i, buf, 1);
      }
    }
  }
  return SCSI_SUCCESS;
}
/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Executes SCSI command encoded in byte array.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] cmd     pointer to SCSI command data
 *
 * @return            The operation status.
 *
 * @api
 */
bool scsiExecCmd(SCSITarget *scsip, const uint8_t *cmd) {

  /* status will be overwritten later in case of error */
  set_sense_ok(scsip);

  switch (cmd[0]) {
  case SCSI_CMD_INQUIRY:
    //chprintf(SDDBG, "SCSI_CMD_INQUIRY\r\n");
    return inquiry(scsip, cmd);

  case SCSI_CMD_REQUEST_SENSE:
    //chprintf(SDDBG, "SCSI_CMD_REQUEST_SENSE\r\n");
    return request_sense(scsip, cmd);

  case SCSI_CMD_READ_CAPACITY_10:
    //chprintf(SDDBG, "SCSI_CMD_READ_CAPACITY_10\r\n");
    return read_capacity10(scsip, cmd);

  case SCSI_CMD_READ_10:
    //chprintf(SDDBG, "SCSI_CMD_READ_10\r\n");
    return data_read_write10(scsip, cmd);

  case SCSI_CMD_WRITE_10:
    //chprintf(SDDBG, "SCSI_CMD_WRITE_10\r\n");
    return data_read_write10(scsip, cmd);

  case SCSI_CMD_TEST_UNIT_READY:
    //chprintf(SDDBG, "SCSI_CMD_TEST_UNIT_READY\r\n");
    return cmd_ignored(scsip, cmd);

  case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
    //chprintf(SDDBG, "SCSI_CMD_ALLOW_MEDIUM_REMOVAL\r\n");
    return cmd_ignored(scsip, cmd);

  case SCSI_CMD_MODE_SENSE_6:
    //chprintf(SDDBG, "SCSI_CMD_MODE_SENSE_6\r\n");
    return mode_sense6(scsip, cmd);

  default:
    //(SDDBG, "SCSI unhandled command: %d\r\n", cmd[0]);
    return cmd_unhandled(scsip, cmd);
  }
}

/**
 * @brief   Driver structure initialization.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 *
 * @api
 */
void scsiObjectInit(SCSITarget *scsip) {

  scsip->config = NULL;
  scsip->residue = 0;
  memset(&scsip->sense, 0 , sizeof(scsi_sense_response_t));
  scsip->state = SCSI_TRGT_STOP;
}

/**
 * @brief   Starts SCSITarget driver.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 * @param[in] config  pointer to @p SCSITargetConfig structure
 *
 * @api
 */
void scsiStart(SCSITarget *scsip, const SCSITargetConfig *config) {

  scsip->config = config;
  scsip->state = SCSI_TRGT_READY;
}

/**
 * @brief   Stops SCSITarget driver.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 *
 * @api
 */
void scsiStop(SCSITarget *scsip) {

  scsip->config = NULL;
  scsip->state = SCSI_TRGT_STOP;
}

/**
 * @brief   Retrieves residue bytes.
 *
 * @param[in] scsip   pointer to @p SCSITarget structure
 *
 * @return            Residue bytes.
 *
 * @api
 */
uint32_t scsiResidue(const SCSITarget *scsip) {

  return scsip->residue;
}

/** @} */