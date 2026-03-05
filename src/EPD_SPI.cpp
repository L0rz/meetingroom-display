#include "EPD_SPI.h"
#include <SPI.h>

/**
 * Bit-Banging SPI — exakt wie Elecrow Demo.
 * Die Demo nutzt KEIN SPI.begin() — nur GPIO toggles.
 */
void EPD_GPIOInit(void)
{
  pinMode(SCK, OUTPUT);
  pinMode(MOSI, OUTPUT);
  pinMode(RES, OUTPUT);
  pinMode(DC, OUTPUT);
  pinMode(CS, OUTPUT);
  pinMode(BUSY, INPUT);
}

void EPD_WR_Bus(uint8_t dat)
{
  uint8_t i;
  EPD_CS_Clr();
  for (i = 0; i < 8; i++)
  {
    EPD_SCK_Clr();
    if (dat & 0x80)
    {
      EPD_MOSI_Set();
    }
    else
    {
      EPD_MOSI_Clr();
    }
    EPD_SCK_Set();
    dat <<= 1;
  }
  EPD_CS_Set();
}

void SPI_Write(unsigned char value)
{
  // unused in bit-bang mode
  EPD_WR_Bus(value);
}

void EPD_WR_REG(uint8_t reg)
{
  EPD_DC_Clr();
  EPD_WR_Bus(reg);
  EPD_DC_Set();
}

void EPD_WR_DATA8(uint8_t dat)
{
  EPD_DC_Set();
  EPD_WR_Bus(dat);
  EPD_DC_Set();
}

void EPD_WR_DATA_BUF(const uint8_t *buf, size_t len)
{
  EPD_DC_Set();
  for (size_t i = 0; i < len; i++) {
    EPD_WR_Bus(buf[i]);
  }
}
