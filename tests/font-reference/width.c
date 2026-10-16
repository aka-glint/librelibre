#include "u8g2.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv) {
 u8g2_t u={0}; u8g2_SetFont(&u,atoi(argv[1]) ? u8g2_font_helvB08_tr : u8g2_font_haxrcorp4089_tr);
 for(int i=2;i<argc;i++) printf("%u %s\n",u8g2_GetStrWidth(&u,argv[i]),argv[i]);
 return 0;
}
