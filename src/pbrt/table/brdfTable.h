#ifndef BRDFTABLE_H
#define BRDFTABLE_H

// BRDFテーブルのサイズは共通
constexpr int BRDF_WIDTH = 300;
constexpr int BRDF_HEIGHT = 365;
constexpr int BRDF_DEPTH = 300;

// 各 BRDF テーブルの宣言
extern float BRDFTABLE5_1[BRDF_WIDTH][BRDF_HEIGHT][BRDF_DEPTH];
extern float BRDFTABLE5_2[BRDF_WIDTH][BRDF_HEIGHT][BRDF_DEPTH];
extern float BRDFTABLE5_3[BRDF_WIDTH][BRDF_HEIGHT][BRDF_DEPTH];
extern float BRDFTABLE5_4[BRDF_WIDTH][BRDF_HEIGHT][BRDF_DEPTH];

// 現在のテーブルへのポインタを保持するグローバル変数
extern float (*CurrentBRDFTable)[BRDF_HEIGHT][BRDF_DEPTH];

// テーブルを切り替える関数
void SetBRDFTable(float (*newTable)[BRDF_HEIGHT][BRDF_DEPTH]);

#endif // BRDFTABLE_H
