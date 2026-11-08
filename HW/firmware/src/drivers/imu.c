/* SPDX-License-Identifier: MIT */
#include "sf_imu.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
static BMI2_INTF_RETURN_TYPE rd(uint8_t reg, uint8_t *buf, uint32_t n, void *ctx) {
    struct sf_imu *i=ctx; return i->bus.read(i->bus.ctx,0x68,reg,buf,n)?-1:BMI2_INTF_RET_SUCCESS;
}
static BMI2_INTF_RETURN_TYPE wr(uint8_t reg, const uint8_t *buf, uint32_t n, void *ctx) {
    struct sf_imu *i=ctx; return i->bus.write(i->bus.ctx,0x68,reg,buf,n)?-1:BMI2_INTF_RET_SUCCESS;
}
static void delay(uint32_t us, void *ctx) {
    struct sf_imu *i=ctx; i->bus.delay_ms(i->bus.ctx,(us+999u)/1000u);
}
static int shutdown_reg(const struct sf_bus *b, uint8_t reg, uint8_t value, uint8_t mask) {
    /* APS may already be active. Observe its >=450us interface idle time,
     * including the write-to-readback interval, using the millisecond bus API. */
    b->delay_ms(b->ctx,1);
    int err=b->write(b->ctx,0x68,reg,&value,1);
    b->delay_ms(b->ctx,1);
    uint8_t got=0;
    int read_err=b->read(b->ctx,0x68,reg,&got,1);
    return err||read_err||(got&mask)!=(value&mask)?-EIO:0;
}
int sf_imu_shutdown(const struct sf_bus *b) {
    if (!b||!b->read||!b->write||!b->delay_ms) return -EINVAL;
    uint8_t id=0;
    if (b->read(b->ctx,0x68,BMI2_CHIP_ID_ADDR,&id,1)) return -EIO;
    if (id!=BMI270_CHIP_ID) return -ENODEV;
    /* BMI270 DS000-08 sections 4.5, 5.2.49..50, 5.2.58..63, 5.2.84..85.
     * Sensor power is removed first. Continue remaining shutdown attempts on
     * errors, and report failure rather than claiming a confirmed suspend. */
    const uint8_t settings[][3]={
        {BMI2_PWR_CTRL_ADDR,0x00,0x0f},
        {BMI2_PWR_CONF_ADDR,0x00,0x07},
        {BMI2_INT1_IO_CTRL_ADDR,0x00,0x1e},
        {BMI2_INT2_IO_CTRL_ADDR,0x00,0x1e},
        {BMI2_INT1_MAP_FEAT_ADDR,0x00,0x7f},
        {BMI2_INT2_MAP_FEAT_ADDR,0x00,0x7f},
        {BMI2_INT_MAP_DATA_ADDR,0x00,0xff},
        {BMI2_FIFO_CONFIG_0_ADDR,0x00,0x03},
        {BMI2_FIFO_CONFIG_1_ADDR,0x00,0xff},
        {BMI2_IF_CONF_ADDR,0x00,0x30}
    };
    int result=0;
    for (size_t j=0;j<sizeof(settings)/sizeof(settings[0]);j++) {
        if (shutdown_reg(b,settings[j][0],settings[j][1],settings[j][2])) result=-EIO;
    }
    uint8_t flush=BMI2_FIFO_FLUSH_CMD;
    b->delay_ms(b->ctx,1);
    if (b->write(b->ctx,0x68,BMI2_CMD_REG_ADDR,&flush,1)) result=-EIO;
    if (shutdown_reg(b,BMI2_PWR_CONF_ADDR,0x01,0x07)) result=-EIO;
    return result;
}
int sf_imu_init(struct sf_imu *i, const struct sf_bus *bus, bool gyro) {
    memset(i,0,sizeof(*i)); i->bus=*bus; i->gyro=gyro;
    i->dev.intf=BMI2_I2C_INTF; i->dev.intf_ptr=i;
    i->dev.read=rd; i->dev.write=wr; i->dev.delay_us=delay;
    i->dev.read_write_len=128; i->dev.config_file_ptr=NULL;
    if (bmi270_init(&i->dev)!=BMI2_OK) return -ENODEV;
    uint8_t interface;
    if (bmi2_get_regs(BMI2_IF_CONF_ADDR,&interface,1,&i->dev)) return -EIO;
    interface&=(uint8_t)~(BMI2_AUX_IF_EN_MASK|BMI2_OIS_IF_EN_MASK);
    if (bmi2_set_regs(BMI2_IF_CONF_ADDR,&interface,1,&i->dev)||
        bmi2_get_regs(BMI2_IF_CONF_ADDR,&interface,1,&i->dev)||
        (interface&(BMI2_AUX_IF_EN_MASK|BMI2_OIS_IF_EN_MASK))) return -EIO;
    const uint8_t off[]={BMI2_AUX,BMI2_GYRO};
    if (bmi2_sensor_disable(off,gyro?1:2,&i->dev)) return -EIO;
    struct bmi2_sens_config cfg[2]={{.type=BMI2_ACCEL},{.type=BMI2_GYRO}};
    if (bmi2_get_sensor_config(cfg,2,&i->dev)) return -EIO;
    cfg[0].cfg.acc.odr=BMI2_ACC_ODR_100HZ;
    cfg[0].cfg.acc.range=BMI2_ACC_RANGE_16G;
    cfg[0].cfg.acc.bwp=BMI2_ACC_NORMAL_AVG4;
    cfg[0].cfg.acc.filter_perf=BMI2_POWER_OPT_MODE;
    cfg[1].cfg.gyr.odr=BMI2_GYR_ODR_100HZ;
    cfg[1].cfg.gyr.range=BMI2_GYR_RANGE_2000;
    cfg[1].cfg.gyr.bwp=BMI2_GYR_NORMAL_MODE;
    cfg[1].cfg.gyr.noise_perf=BMI2_POWER_OPT_MODE;
    cfg[1].cfg.gyr.filter_perf=BMI2_POWER_OPT_MODE;
    if (bmi2_set_sensor_config(cfg,2,&i->dev)) return -EIO;
    uint8_t enabled[]={BMI2_ACCEL,BMI2_GYRO};
    if (bmi2_sensor_enable(enabled,gyro?2:1,&i->dev)) return -EIO;
    /* FIFO access requires APS disabled; acceleration retains power-optimized
     * filtering. No AUX/OIS peripheral is enabled. */
    if (bmi2_set_adv_power_save(BMI2_DISABLE,&i->dev)) return -EIO;
    struct bmi2_int_pin_config pin={.pin_type=BMI2_INT1,.int_latch=BMI2_INT_NON_LATCH};
    pin.pin_cfg[0].lvl=BMI2_INT_ACTIVE_HIGH;
    pin.pin_cfg[0].od=BMI2_INT_PUSH_PULL;
    pin.pin_cfg[0].output_en=BMI2_INT_OUTPUT_ENABLE;
    if (bmi2_set_int_pin_config(&pin,&i->dev)) return -EIO;
    if (bmi2_set_fifo_config(BMI2_FIFO_ALL_EN,BMI2_DISABLE,&i->dev)) return -EIO;
    uint16_t fifo=(uint16_t)(BMI2_FIFO_ACC_EN|BMI2_FIFO_HEADER_EN|BMI2_FIFO_STOP_ON_FULL|
                           (gyro?BMI2_FIFO_GYR_EN:0));
    if (bmi2_set_fifo_config(fifo,BMI2_ENABLE,&i->dev)||
        bmi2_set_fifo_wm(gyro?52:28,&i->dev)||
        bmi2_map_data_int(BMI2_FWM_INT|BMI2_FFULL_INT,BMI2_INT1,&i->dev)) return -EIO;
    uint8_t flush=BMI2_FIFO_FLUSH_CMD;
    return wr(BMI2_CMD_REG_ADDR,&flush,1,i)?-EIO:0;
}
int sf_imu_read(struct sf_imu *i, struct sf_sample *out, size_t cap,
                size_t *count, uint32_t now) {
    *count=0;
    if (!cap||cap>64) return -EINVAL;
    uint16_t len=0;
    if (bmi2_get_fifo_length(&len,&i->dev)) return -EIO;
    if (!len) return 0;
    if (len>512||len>cap*(i->gyro?13u:7u)) {
        uint8_t flush=BMI2_FIFO_FLUSH_CMD;
        (void)wr(BMI2_CMD_REG_ADDR,&flush,1,i); return -EOVERFLOW;
    }
    uint8_t raw[512];
    struct bmi2_fifo_frame fifo={.data=raw,.length=len};
    if (bmi2_read_fifo_data(&fifo,&i->dev)) return -EIO;
    struct bmi2_sens_axes_data acc[64], gyr[64];
    uint16_t na=(uint16_t)cap, ng=(uint16_t)cap;
    if (bmi2_extract_accel(acc,&na,&fifo,&i->dev)<0) return -EIO;
    if (i->gyro&&bmi2_extract_gyro(gyr,&ng,&fifo,&i->dev)<0) return -EIO;
    if ((i->gyro&&na!=ng)||fifo.skipped_frame_count) return -EOVERFLOW;
    for (uint16_t j=0;j<na;j++) {
        out[j]=(struct sf_sample){.a={acc[j].x,acc[j].y,acc[j].z},
            .time_ms=now-(uint32_t)(na-1-j)*10u,.flags=SF_TIME_ESTIMATED};
        if (i->gyro) {
            out[j].g[0]=gyr[j].x; out[j].g[1]=gyr[j].y; out[j].g[2]=gyr[j].z;
            out[j].flags|=SF_GYRO_VALID;
        }
        for (unsigned k=0;k<3;k++) if (out[j].a[k]==INT16_MIN||out[j].a[k]==INT16_MAX)
            out[j].flags|=SF_SATURATED;
    }
    *count=na; return 0;
}
