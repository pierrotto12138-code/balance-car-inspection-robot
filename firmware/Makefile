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
            -IApp/Board -IApp/Control -IApp/Config -IApp/Filter -IApp/Ui \
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
# ★ 显式声明默认目标：Make 取"文件里第一个目标"当默认，一旦有人在 all 之前
#   插入别的目标（本文件就发生过一次：compile_commands 插到 all 前面，
#   结果裸跑 `make` 变成只生成 JSON、**不再构建固件**），行为就悄悄变了。
#   写死这一行，插入新目标就不会再踩。
.DEFAULT_GOAL := all

.PHONY: all clean size flash strict compile_commands

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

# ----------------------------------------------------------------------------
# 生成 compile_commands.json（给编辑器做代码跳转/补全用）
#   做法：抓 `make -n` 里真实的编译命令行，不真编译、不依赖 bear/compiledb。
#   编辑器（VS Code + C/C++ 或 clangd）读它之后，`-I`/`-D` 与真实构建完全一致，
#   不会出现"找不到 stm32f4xx.h / FreeRTOS.h"的假报错。
#   用法：make compile_commands  →  firmware/compile_commands.json（本机生成物，已 gitignore）
#   ★ 注意：本目标必须留在 `all` **之后**（Make 的默认目标是文件里第一个目标；
#     上面已用 .DEFAULT_GOAL 兜底，但保持这个顺序更直观）。
# ----------------------------------------------------------------------------
compile_commands:
	python ../tools/fw_prep/gen_compile_commands.py

# ----------------------------------------------------------------------------
# 严格告警自检：只编**我们自研/手改**的文件，且把厂商目录用 -isystem 屏蔽掉
#   —— 这样输出里剩下的每一条告警都是我们自己的责任。
#   为什么单独一个目标：`-Wall -Wextra` 挡不住类型窄化、隐式符号转换、缺原型这类问题；
#   而它们恰恰是最容易在换编译器/换优化等级时突然咬人的一类。
#   参考：docs/design_firmware/编码规范.md
# ----------------------------------------------------------------------------
STRICT_WARN := -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wdouble-promotion \
               -Wundef -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
               -Wredundant-decls -Wformat=2
STRICT_SRC  := App/Task/app_tasks.c App/Task/patrol.c App/Task/odom.c \
               App/Board/board_state.c \
               App/Comms/protocol.c App/Comms/crc16.c App/Comms/ringbuf.c App/Comms/comm_link.c \
               App/Sensor/gas.c App/Sensor/dht11.c App/Sensor/dht11_hw.c \
               App/Sensor/sensor_report.c App/Sensor/sensor_adc.c \
               App/Control/param_pack.c App/Control/param_store.c \
               App/Ui/pages.c App/Ui/buzzer.c \
               User/main.c User/rtos_hooks.c
# 我们自己的头目录用 -I（要看到自己头文件里的问题）；厂商目录用 -isystem（屏蔽其告警）
STRICT_INC  := $(foreach d,$(filter-out Drivers/% Middlewares/%,$(patsubst -I%,%,$(INCS))),-I$(d))
STRICT_SYS  := $(foreach d,$(filter Drivers/% Middlewares/%,$(patsubst -I%,%,$(INCS))),-isystem $(d))

strict:
	@echo "=== 严格告警自检（$(words $(STRICT_SRC)) 个自研/手改文件；厂商头用 -isystem 屏蔽）==="
	@fail=0; total=0; \
	for f in $(STRICT_SRC); do \
		n=$$($(CC) -fsyntax-only $(CFLAGS) $(STRICT_WARN) $(STRICT_SYS) $(STRICT_INC) $$f 2>&1 \
			| grep -c 'warning:\|error:' || true); \
		if [ "$$n" != "0" ]; then echo "  [$$n 条] $$f"; \
		$(CC) -fsyntax-only $(CFLAGS) $(STRICT_WARN) $(STRICT_SYS) $(STRICT_INC) $$f 2>&1 \
			| grep 'warning:\|error:' | head -3 | sed 's/^/        /'; fail=1; fi; \
		total=$$((total + n)); \
	done; \
	echo "=== 合计 $$total 条 ==="; \
	if [ "$$fail" = "0" ]; then echo "自研代码在严格告警下 0 条 ✅"; \
	else echo "有告警，请按 docs/design_firmware/编码规范.md 处理 ❌"; exit 1; fi

clean:
	-rm -fR $(BUILD)
