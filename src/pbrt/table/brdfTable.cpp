#include "brdfTable.h"

// 現在使用しているBRDFテーブルへのポインタ
float (*CurrentBRDFTable)[BRDF_HEIGHT][BRDF_DEPTH] = BRDFTABLE5_1;  // デフォルトは BRDFTABLE5_1

// BRDFテーブルを切り替える関数
void SetBRDFTable(float (*newTable)[BRDF_HEIGHT][BRDF_DEPTH]) {
    CurrentBRDFTable = newTable;
}
