# ============================================================================
#  STM32F405RGT6 固件构建（GCC / arm-none-eabi，决策 34）
#
#  为什么不用 Keil：本项目不使用 Keil；CubeCLT 自带 arm-none-eabi-gcc 14.3，
#  实测可构建 F405 镜像。烧录用板载 CH340G 走串口 ISP（FlyMcu 烧 .hex）。
#
#  常用：
#     make            生成 build/*.elf + *.hex + *.bin + *.map
#     make size       看 Flash/RAM 占用
#     make flash      ST-Link 下载（需接 ST-Link）
#     make clean      清理
# ============================================================================

TARGET   := MiniBalance
BUILD    := build

# ---- 工具链 ----
# PREFIX 用 `?=` ：工具链装在哪都行，既可以用 PATH，
# 也可以 `make PREFIX=/path/to/bin/arm-none-eabi-`（或 `make CC=... `）指定。
PREFIX   ?= arm-none-eabi-
CC       := $(PREFIX)gcc
AS       := $(PREFIX)gcc -x assembler-with-cpp
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size
HEX      := $(OBJCOPY) -O ihex
BIN      := $(OBJCOPY) -O binary -S

# ---- 芯片 ----
CPU      := -mcpu=cortex-m4
FPU      := -mfpu=fpv4-sp-d16
FLOAT    := -mfloat-abi=hard
MCU      := $(CPU) -mthumb $(FPU) $(FLOAT)

# ---- 宏 ----
# USE_RTOS=1（默认）：启用 FreeRTOS（混合式架构，决策 8）
# USE_RTOS=0        ：纯裸机（只用于对照排错，例如怀疑 RTOS 影响平衡时二分定位）
USE_RTOS ?= 1
ifeq ($(USE_RTOS),1)
  RTOS_DEF  := -DUSE_FREERTOS
  RTOS_INC  := -IMiddlewares/FreeRTOS/include -IMiddlewares/FreeRTOS/portable/GCC/ARM_CM4F
  RTOS_SRC  := $(wildcard Middlewares/FreeRTOS/Core/*.c) \
               Middlewares/FreeRTOS/portable/GCC/ARM_CM4F/port.c \
               Middlewares/FreeRTOS/portable/MemMang/heap_4.c \
               User/rtos_hooks.c
else
  RTOS_DEF  :=
  RTOS_INC  :=
  RTOS_SRC  :=
endif

DEFS     := -DUSE_STDPERIPH_DRIVER -DSTM32F40_41xxx $(RTOS_DEF)

# ---- 头文件路径 ----
INCS     := -IDrivers/CMSIS \
            -IDrivers/FWLIB/inc \
            -IUser \
            -ISystem/sys -ISystem/delay -ISystem/usart -ISystem/retarget \
            -IApp/Control -IApp/Config -IApp/Filter -IApp/Ui \
            -IApp/Sensor -IApp/Task \
            -IApp/Sensor/MPU6050 -IApp/Sensor/DMP -IApp/Comms -IApp/Comms/DataScope \
            -IDrivers/BSP/ADC -IDrivers/BSP/CAN -IDrivers/BSP/ENCODER \
            -IDrivers/BSP/EXTI -IDrivers/BSP/IIC -IDrivers/BSP/KEY \
            -IDrivers/BSP/LED -IDrivers/BSP/MOTOR -IDrivers/BSP/OLED \
            -IDrivers/BSP/SPI -IDrivers/BSP/STMFLASH -IDrivers/BSP/TIMER \
            -IDrivers/BSP/USARTX -IDrivers/BSP/NRF24L01 \
            $(RTOS_INC)

# ---- 编译选项 ----
# 说明：基线源码已由 tools/fw_prep/vendor_baseline.py 转成 UTF-8，故无需 -finput-charset
CFLAGS   := $(MCU) $(DEFS) $(INCS) \
            -Og -g3 -gdwarf-2 -Wall -Wextra \
            -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
            -fno-common -ffunction-sections -fdata-sections \
            -std=gnu11
ASFLAGS  := $(MCU) $(DEFS) $(INCS) -g3 -gdwarf-2

# ---- 链接选项 ----
LDSCRIPT := Drivers/CMSIS/stm32f405rg_flash.ld
LIBS     := -lc -lm -lnosys
LDFLAGS  := $(MCU) -specs=nano.specs -specs=nosys.specs \
            -T$(LDSCRIPT) $(LIBS) \
            -Wl,-Map=$(BUILD)/$(TARGET).map,--cref \
            -Wl,--gc-sections -Wl,--print-memory-usage

# ---- 源文件 ----
# 说明：FWLIB 里的 stm32f4xx_fmc.c 是 F42x/43x 才有的 FMC 外设，F405 上会编译失败，
#       属于"本来就该剔除"，不是缺陷（见 docs/design_firmware/B阶段开工准备.md §三-2）。
C_SOURCES := \
  $(wildcard Drivers/FWLIB/src/*.c) \
  $(wildcard Drivers/BSP/*/*.c) \
  $(wildcard System/*/*.c) \
  $(wildcard App/*/*.c) \
  $(wildcard App/*/*/*.c) \
  $(RTOS_SRC) \
  User/main.c User/stm32f4xx_it.c User/system_stm32f4xx.c

C_SOURCES := $(filter-out Drivers/FWLIB/src/stm32f4xx_fmc.c,$(C_SOURCES))

ASM_SOURCES := Drivers/CMSIS/startup_stm32f405.S

OBJECTS := $(addprefix $(BUILD)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD)/,$(notdir $(ASM_SOURCES:.S=.o)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

# ============================================================================
.PHONY: all clean size flash

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.S Makefile | $(BUILD)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(HEX) $< $@
	@echo "  hex 就绪 → $@（可直接用 FlyMcu 串口 ISP 烧录）"

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(BIN) $< $@

$(BUILD):
	mkdir -p $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) -A -x $< | head -40
	@$(SIZE) $<

# ST-Link 下载（需要接 ST-Link；串口 ISP 请用 FlyMcu 载入 .hex）
flash: $(BUILD)/$(TARGET).hex
	STM32_Programmer_CLI -c port=SWD -d $< -v -rst

clean:
	-rm -fR $(BUILD)
