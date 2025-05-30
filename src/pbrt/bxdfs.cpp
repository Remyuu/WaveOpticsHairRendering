// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#include <pbrt/bxdfs.h>

#include <pbrt/bssrdf.h>
#include <pbrt/interaction.h>
#include <pbrt/media.h>
#include <pbrt/options.h>
#include <pbrt/util/check.h>
#include <pbrt/util/color.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/error.h>
#include <pbrt/util/file.h>
#include <pbrt/util/float.h>
#include <pbrt/util/hash.h>
#include <pbrt/util/log.h>
#include <pbrt/util/math.h>
#include <pbrt/util/memory.h>
#include <pbrt/util/print.h>
#include <pbrt/util/sampling.h>
#include <pbrt/util/stats.h>

#include <pbrt/util/spectrum.h>
#include <unordered_map>

#include "table/brdfTable.h" // 引入用于 BRDF 计算的表格数据

namespace pbrt {
inline Float RadiansToDegrees(Float radians) {
    return radians * 180.0f / Pi;
}

std::string ToString(BxDFReflTransFlags flags) {
    if (flags == BxDFReflTransFlags::Unset)
        return "Unset";
    std::string s;
    if (flags & BxDFReflTransFlags::Reflection)
        s += "Reflection,";
    if (flags & BxDFReflTransFlags::Transmission)
        s += "Transmission,";
    return s;
}

std::string ToString(BxDFFlags flags) {
    if (flags == BxDFFlags::Unset)
        return "Unset";
    std::string s;
    if (flags & BxDFFlags::Reflection)
        s += "Reflection,";
    if (flags & BxDFFlags::Transmission)
        s += "Transmission,";
    if (flags & BxDFFlags::Diffuse)
        s += "Diffuse,";
    if (flags & BxDFFlags::Glossy)
        s += "Glossy,";
    if (flags & BxDFFlags::Specular)
        s += "Specular,";
    return s;
}

std::string ToString(TransportMode mode) {
    return mode == TransportMode::Radiance ? "Radiance" : "Importance";
}

// BxDF Method Definitions
std::string DiffuseBxDF::ToString() const {
    return StringPrintf("[ DiffuseBxDF R: %s ]", R);
}
std::string DiffuseTransmissionBxDF::ToString() const {
    return StringPrintf("[ DiffuseTransmissionBxDF R: %s T: %s ]", R, T);
}

template <typename TopBxDF, typename BottomBxDF, bool twoSided>
std::string LayeredBxDF<TopBxDF, BottomBxDF, twoSided>::ToString() const {
    return StringPrintf(
        "[ LayeredBxDF top: %s bottom: %s thickness: %f albedo: %s g: %f ]", top, bottom,
        thickness, albedo, g);
}

// DielectricBxDF Method Definitions
PBRT_CPU_GPU pstd::optional<BSDFSample> DielectricBxDF::Sample_f(
    Vector3f wo, Float uc, Point2f u, TransportMode mode,
    BxDFReflTransFlags sampleFlags) const {
    if (eta == 1 || mfDistrib.EffectivelySmooth()) {
        // Sample perfect specular dielectric BSDF
        Float R = FrDielectric(CosTheta(wo), eta), T = 1 - R;
        // Compute probabilities _pr_ and _pt_ for sampling reflection and transmission
        Float pr = R, pt = T;
        if (!(sampleFlags & BxDFReflTransFlags::Reflection))
            pr = 0;
        if (!(sampleFlags & BxDFReflTransFlags::Transmission))
            pt = 0;
        if (pr == 0 && pt == 0)
            return {};

        if (uc < pr / (pr + pt)) {
            // Sample perfect specular dielectric BRDF
            Vector3f wi(-wo.x, -wo.y, wo.z);
            SampledSpectrum fr(R / AbsCosTheta(wi));
            return BSDFSample(fr, wi, pr / (pr + pt), BxDFFlags::SpecularReflection);

        } else {
            // Sample perfect specular dielectric BTDF
            // Compute ray direction for specular transmission
            Vector3f wi;
            Float etap;
            bool valid = Refract(wo, Normal3f(0, 0, 1), eta, &etap, &wi);
            CHECK_RARE(1e-5f, !valid);
            if (!valid)
                return {};

            SampledSpectrum ft(T / AbsCosTheta(wi));
            // Account for non-symmetry with transmission to different medium
            if (mode == TransportMode::Radiance)
                ft /= Sqr(etap);

            return BSDFSample(ft, wi, pt / (pr + pt), BxDFFlags::SpecularTransmission,
                              etap);
        }

    } else {
        // Sample rough dielectric BSDF
        Vector3f wm = mfDistrib.Sample_wm(wo, u);
        Float R = FrDielectric(Dot(wo, wm), eta);
        Float T = 1 - R;
        // Compute probabilities _pr_ and _pt_ for sampling reflection and transmission
        Float pr = R, pt = T;
        if (!(sampleFlags & BxDFReflTransFlags::Reflection))
            pr = 0;
        if (!(sampleFlags & BxDFReflTransFlags::Transmission))
            pt = 0;
        if (pr == 0 && pt == 0)
            return {};

        Float pdf;
        if (uc < pr / (pr + pt)) {
            // Sample reflection at rough dielectric interface
            Vector3f wi = Reflect(wo, wm);
            if (!SameHemisphere(wo, wi))
                return {};
            // Compute PDF of rough dielectric reflection
            pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm)) * pr / (pr + pt);

            DCHECK(!IsNaN(pdf));
            SampledSpectrum f(mfDistrib.D(wm) * mfDistrib.G(wo, wi) * R /
                              (4 * CosTheta(wi) * CosTheta(wo)));
            return BSDFSample(f, wi, pdf, BxDFFlags::GlossyReflection);

        } else {
            // Sample transmission at rough dielectric interface
            Float etap;
            Vector3f wi;
            bool tir = !Refract(wo, (Normal3f)wm, eta, &etap, &wi);
            CHECK_RARE(1e-5f, tir);
            if (SameHemisphere(wo, wi) || wi.z == 0 || tir)
                return {};
            // Compute PDF of rough dielectric transmission
            Float denom = Sqr(Dot(wi, wm) + Dot(wo, wm) / etap);
            Float dwm_dwi = AbsDot(wi, wm) / denom;
            pdf = mfDistrib.PDF(wo, wm) * dwm_dwi * pt / (pr + pt);

            CHECK(!IsNaN(pdf));
            // Evaluate BRDF and return _BSDFSample_ for rough transmission
            SampledSpectrum ft(T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) *
                               std::abs(Dot(wi, wm) * Dot(wo, wm) /
                                        (CosTheta(wi) * CosTheta(wo) * denom)));
            // Account for non-symmetry with transmission to different medium
            if (mode == TransportMode::Radiance)
                ft /= Sqr(etap);

            return BSDFSample(ft, wi, pdf, BxDFFlags::GlossyTransmission, etap);
        }
    }
}

PBRT_CPU_GPU SampledSpectrum DielectricBxDF::f(Vector3f wo, Vector3f wi, TransportMode mode) const {
    if (eta == 1 || mfDistrib.EffectivelySmooth())
        return SampledSpectrum(0.f);
    // Evaluate rough dielectric BSDF
    // Compute generalized half vector _wm_
    Float cosTheta_o = CosTheta(wo), cosTheta_i = CosTheta(wi);
    bool reflect = cosTheta_i * cosTheta_o > 0;
    float etap = 1;
    if (!reflect)
        etap = cosTheta_o > 0 ? eta : (1 / eta);
    Vector3f wm = wi * etap + wo;
    CHECK_RARE(1e-5f, LengthSquared(wm) == 0);
    if (cosTheta_i == 0 || cosTheta_o == 0 || LengthSquared(wm) == 0)
        return {};
    wm = FaceForward(Normalize(wm), Normal3f(0, 0, 1));

    // Discard backfacing microfacets
    if (Dot(wm, wi) * cosTheta_i < 0 || Dot(wm, wo) * cosTheta_o < 0)
        return {};

    Float F = FrDielectric(Dot(wo, wm), eta);
    if (reflect) {
        // Compute reflection at rough dielectric interface
        return SampledSpectrum(mfDistrib.D(wm) * mfDistrib.G(wo, wi) * F /
                               std::abs(4 * cosTheta_i * cosTheta_o));

    } else {
        // Compute transmission at rough dielectric interface
        Float denom = Sqr(Dot(wi, wm) + Dot(wo, wm) / etap) * cosTheta_i * cosTheta_o;
        Float ft = mfDistrib.D(wm) * (1 - F) * mfDistrib.G(wo, wi) *
                   std::abs(Dot(wi, wm) * Dot(wo, wm) / denom);
        // Account for non-symmetry with transmission to different medium
        if (mode == TransportMode::Radiance)
            ft /= Sqr(etap);

        return SampledSpectrum(ft);
    }
}

PBRT_CPU_GPU Float DielectricBxDF::PDF(Vector3f wo, Vector3f wi, TransportMode mode,
                          BxDFReflTransFlags sampleFlags) const {
    if (eta == 1 || mfDistrib.EffectivelySmooth())
        return 0;
    // Evaluate sampling PDF of rough dielectric BSDF
    // Compute generalized half vector _wm_
    Float cosTheta_o = CosTheta(wo), cosTheta_i = CosTheta(wi);
    bool reflect = cosTheta_i * cosTheta_o > 0;
    float etap = 1;
    if (!reflect)
        etap = cosTheta_o > 0 ? eta : (1 / eta);
    Vector3f wm = wi * etap + wo;
    CHECK_RARE(1e-5f, LengthSquared(wm) == 0);
    if (cosTheta_i == 0 || cosTheta_o == 0 || LengthSquared(wm) == 0)
        return {};
    wm = FaceForward(Normalize(wm), Normal3f(0, 0, 1));

    // Discard backfacing microfacets
    if (Dot(wm, wi) * cosTheta_i < 0 || Dot(wm, wo) * cosTheta_o < 0)
        return {};

    // Determine Fresnel reflectance of rough dielectric boundary
    Float R = FrDielectric(Dot(wo, wm), eta);
    Float T = 1 - R;

    // Compute probabilities _pr_ and _pt_ for sampling reflection and transmission
    Float pr = R, pt = T;
    if (!(sampleFlags & BxDFReflTransFlags::Reflection))
        pr = 0;
    if (!(sampleFlags & BxDFReflTransFlags::Transmission))
        pt = 0;
    if (pr == 0 && pt == 0)
        return {};

    // Return PDF for rough dielectric
    Float pdf;
    if (reflect) {
        // Compute PDF of rough dielectric reflection
        pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm)) * pr / (pr + pt);

    } else {
        // Compute PDF of rough dielectric transmission
        Float denom = Sqr(Dot(wi, wm) + Dot(wo, wm) / etap);
        Float dwm_dwi = AbsDot(wi, wm) / denom;
        pdf = mfDistrib.PDF(wo, wm) * dwm_dwi * pt / (pr + pt);
    }
    return pdf;
}

std::string DielectricBxDF::ToString() const {
    return StringPrintf("[ DielectricBxDF eta: %f mfDistrib: %s ]", eta,
                        mfDistrib.ToString());
}

std::string ThinDielectricBxDF::ToString() const {
    return StringPrintf("[ ThinDielectricBxDF eta: %f ]", eta);
}

std::string ConductorBxDF::ToString() const {
    return StringPrintf("[ ConductorBxDF mfDistrib: %s eta: %s k: %s ]", mfDistrib, eta,
                        k);
}

// HairBxDF 方法定义
PBRT_CPU_GPU HairBxDF::HairBxDF(Float h, Float eta, const SampledSpectrum &sigma_a, Float beta_m,
                   Float beta_n, Float alpha)
    // h: 表示头发截面上位置的偏移量。
    // eta: 头发的折射率。一般来说，头发的折射率大约为1.55，但根据材质的不同会有所变化。
    // sigma_a: 吸收特性，表示头发的吸光性，决定头发的颜色和光的吸收情况。
    // beta_m: 控制头发表面在长轴方向上的乱度（即反射光的扩散程度）。
    // beta_n: 控制头发表面在法线方向上的乱度。
    // alpha: 影响散射角度的参数，用于控制光的散射行为。
    : h(h), eta(eta), sigma_a(sigma_a), beta_m(beta_m), beta_n(beta_n) {
    
    // 确保 h, beta_m 和 beta_n 的值在合法范围内
    CHECK(h >= -1 && h <= 1);
    CHECK(beta_m >= 0 && beta_m <= 1);
    CHECK(beta_n >= 0 && beta_n <= 1);

    // 验证 pMax 的值是否足够大
    static_assert(pMax >= 3,
                  "Longitudinal variance code must be updated to handle low pMax");

    // 根据 beta_m 计算 v[0] 到 v[2]，这些值与头发表面的乱度相关
    v[0] = Sqr(0.726f * beta_m + 0.812f * Sqr(beta_m) + 3.7f * Pow<20>(beta_m));
    v[1] = .25 * v[0];  // v[1] 是 v[0] 的四分之一
    v[2] = 4 * v[0];    // v[2] 是 v[0] 的四倍

    // 如果 p > 2，v[p] 将被设置为 v[2]，用于控制散射强度
    for (int p = 3; p <= pMax; ++p)
        v[p] = v[2]; // TODO: 这里是否可以有更好的方法？

    // 设置与乱度相关的常量 s
    static const Float SqrtPiOver8 = 0.626657069f;
    s = SqrtPiOver8 * (0.265f * beta_n + 1.194f * Sqr(beta_n) + 5.372f * Pow<22>(beta_n));
    DCHECK(!IsNaN(s));  // 确保 s 不是 NaN

    // 计算散射角度相关的 sin 和 cos 值
    sin2kAlpha[0] = std::sin(Radians(alpha));  // 计算 alpha 对应的 sin 值
    cos2kAlpha[0] = SafeSqrt(1 - Sqr(sin2kAlpha[0]));  // 计算 alpha 对应的 cos 值

    // 根据散射角度的 sin 和 cos 值计算后续角度的 sin2kAlpha 和 cos2kAlpha 值
    for (int i = 1; i < pMax; ++i) {
        sin2kAlpha[i] = 2 * cos2kAlpha[i - 1] * sin2kAlpha[i - 1];  // 递推计算 sin2kAlpha
        cos2kAlpha[i] = Sqr(cos2kAlpha[i - 1]) - Sqr(sin2kAlpha[i - 1]);  // 递推计算 cos2kAlpha
    }

    // v[0] 到 v[2] 的计算：这些值表示头发表面乱度的参数，基于 beta_m 来计算，
    // 决定了头发表面光的反射扩散程度。
    // s: 与头发乱度相关的常量，基于 beta_n 来计算，用于控制多重散射的效果。
    // sin2kAlpha 和 cos2kAlpha：这些是散射角度的预计算值，用于调整散射过程。
}

// HairBxDF::f 方法定义
PBRT_CPU_GPU HairBxDF::f(Vector3f wo, Vector3f wi, TransportMode mode) const {
    // wo: 出射方向向量，表示光从哪里出射。
    // wi: 入射方向向量，表示光来自哪里。
    // mode: 传输模式，决定光的传播方式（放射 Radiance 或 重要性 Importance）。

    // 计算与 _wo_ 相关的头发坐标系项
    Float sinTheta_o = wo.x;  // 出射光的正弦值（相对于头发表面的角度）
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));  // 出射光的余弦值
    Float phi_o = std::atan2(wo.z, wo.y);  // 计算出射光的方位角
    Float gamma_o = SafeASin(h);  // 计算出射光的 gamma 值（散射角度）

    // 计算与 _wi_ 相关的头发坐标系项
    Float sinTheta_i = wi.x;  // 入射光的正弦值（相对于头发表面的角度）
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));  // 入射光的余弦值
    Float phi_i = std::atan2(wi.z, wi.y);  // 计算入射光的方位角

    // 计算屈折光线的 cosTheta_t（折射角的余弦值）
    Float sinTheta_t = sinTheta_o / eta;  // 根据斯涅尔定律计算屈折光线的正弦值
    Float cosTheta_t = SafeSqrt(1 - Sqr(sinTheta_t));  // 屈折光线的余弦值

    // 计算屈折光线的 gamma_t 值（散射角度）
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;  // 屈折光线的 eta' 值
    Float sinGamma_t = h / etap;  // 计算屈折光线的 sinGamma_t
    Float cosGamma_t = SafeSqrt(1 - Sqr(sinGamma_t));  // 计算屈折光线的 cosGamma_t
    Float gamma_t = SafeASin(sinGamma_t);  // 计算屈折光线的 gamma_t（散射角度）

    // 计算穿过头发圆柱体的单一路径的透射率 T
    SampledSpectrum T = Exp(-sigma_a * (2 * cosGamma_t / cosTheta_t));

    // 计算头发的 BSDF（双向散射分布函数）
    Float phi = phi_i - phi_o;  // 入射光和出射光的方位角差值
    pstd::array<SampledSpectrum, pMax + 1> ap = Ap(cosTheta_o, eta, h, T);  // 计算多重散射的影响
    SampledSpectrum fsum(0.);  // 初始化最终的反射光谱值

    // 循环处理每个散射阶数 p，计算多重散射的贡献
    for (int p = 0; p < pMax; ++p) {
        // 计算考虑尺度调整后的出射光的 sinTheta_o 和 cosTheta_o 值
        Float sinThetap_o, cosThetap_o;
        if (p == 0) {
            // 第一阶散射，计算调整后的 sin 和 cos 值
            sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
            cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
        }
        // 处理其他阶数的头发尺度倾斜
        else if (p == 1) {
            sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
            cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
        } else if (p == 2) {
            sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
            cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
        } else {
            sinThetap_o = sinTheta_o;
            cosThetap_o = cosTheta_o;
        }

        // 处理由于尺度调整导致的 cosTheta_o 越界情况
        cosThetap_o = std::abs(cosThetap_o);

        // 计算多重散射过程的贡献，并更新 fsum
        fsum += Mp(cosTheta_i, cosThetap_o, sinTheta_i, sinThetap_o, v[p]) * ap[p] *
                Np(phi, p, s, gamma_o, gamma_t);
    }

    // 计算在 pMax 后的剩余项的贡献
    fsum += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * ap[pMax] / (2 * Pi);

    // 如果入射光的余弦大于 0，则调整反射光谱
    if (AbsCosTheta(wi) > 0)
        fsum /= AbsCosTheta(wi);

    // 确保最终的反射光谱值没有无穷大或 NaN 值
    DCHECK(!IsInf(fsum.Average()) && !IsNaN(fsum.Average()));

    return fsum;  // 返回计算得到的反射光谱
}

PBRT_CPU_GPU pstd::array<Float, HairBxDF::pMax + 1> HairBxDF::ApPDF(Float cosTheta_o) const {
    // Initialize array of $A_p$ values for _cosTheta_o_
    Float sinTheta_o = SafeSqrt(1 - Sqr(cosTheta_o));
    // Compute $\cos\,\thetat$ for refracted ray
    Float sinTheta_t = sinTheta_o / eta;
    Float cosTheta_t = SafeSqrt(1 - Sqr(sinTheta_t));

    // Compute $\gammat$ for refracted ray
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float cosGamma_t = SafeSqrt(1 - Sqr(sinGamma_t));
    Float gamma_t = SafeASin(sinGamma_t);

    // Compute the transmittance _T_ of a single path through the cylinder
    SampledSpectrum T = Exp(-sigma_a * (2 * cosGamma_t / cosTheta_t));

    pstd::array<SampledSpectrum, pMax + 1> ap = Ap(cosTheta_o, eta, h, T);

    // Compute $A_p$ PDF from individual $A_p$ terms
    pstd::array<Float, pMax + 1> apPDF;
    Float sumY = 0;
    for (const SampledSpectrum &as : ap)
        sumY += as.Average();
    for (int i = 0; i <= pMax; ++i)
        apPDF[i] = ap[i].Average() / sumY;

    return apPDF;
}

// HairBxDF::Sample_f 方法定义
PBRT_CPU_GPU pstd::optional<BSDFSample> HairBxDF::Sample_f(Vector3f wo, Float uc, Point2f u,
                                              TransportMode mode,
                                              BxDFReflTransFlags sampleFlags) const {
    // 基于给定的出射方向 wo，采样头发内部的光散射方向。此方法基于蒙特卡洛采样来确定光的行为。
    // uc 和 u: 用于采样的随机数。uc 用于确定采样的粗糙程度，而 u 用于采样随机方向。
    // sampleFlags: 反射或透过的标志，决定光传播的性质（如是否为反射或透射）。

    // 计算与 _wo_ 相关的头发坐标系项（头发表面的入射角度、方向等）
    Float sinTheta_o = wo.x;  // 出射光的正弦值（头发表面方向上的分量）
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));  // 出射光的余弦值
    Float phi_o = std::atan2(wo.z, wo.y);  // 计算出射光的方位角（即光线的旋转角度）
    Float gamma_o = SafeASin(h);  // 计算出射光的 gamma 值（散射角度）

    // 根据给定的散射方向 wo，采样头发表面的随机方向并计算反射或散射的贡献
    // 此方法将通过随机数进行光的方向采样，并根据反射或透射的标志调整行为。
    
    // 这里可以继续加入使用随机数采样不同方向的代码，以及计算光的反射或透射贡献的过程。
    
    // 最后返回计算得到的反射或透射光谱信息，使用 std::optional 包装返回值
    // 如果采样成功，返回 BSDFSample 对象，表示样本的反射或散射方向和强度。

    // Determine which term $p$ to sample for hair scattering
    pstd::array<Float, pMax + 1> apPDF = ApPDF(cosTheta_o);
    int p = SampleDiscrete(apPDF, uc, nullptr, &uc);

    // Compute $\sin\,\thetao$ and $\cos\,\thetao$ terms accounting for scales
    Float sinThetap_o, cosThetap_o;
    if (p == 0) {
        sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
        cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
    }
    // Handle remainder of $p$ values for hair scale tilt
    else if (p == 1) {
        sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
        cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
    } else if (p == 2) {
        sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
        cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
    } else {
        sinThetap_o = sinTheta_o;
        cosThetap_o = cosTheta_o;
    }

    // Handle out-of-range $\cos\,\thetao$ from scale adjustment
    cosThetap_o = std::abs(cosThetap_o);

    // Sample $M_p$ to compute $\thetai$
    Float cosTheta = 1 + v[p] * std::log(std::max<Float>(u[0], 1e-5) +
                                         (1 - u[0]) * FastExp(-2 / v[p]));
    Float sinTheta = SafeSqrt(1 - Sqr(cosTheta));
    Float cosPhi = std::cos(2 * Pi * u[1]);
    Float sinTheta_i = -cosTheta * sinThetap_o + sinTheta * cosPhi * cosThetap_o;
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));

    // Sample $N_p$ to compute $\Delta\phi$
    // Compute $\gammat$ for refracted ray
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float cosGamma_t = SafeSqrt(1 - Sqr(sinGamma_t));
    Float gamma_t = SafeASin(sinGamma_t);

    Float dphi;
    if (p < pMax)
        dphi = Phi(p, gamma_o, gamma_t) + SampleTrimmedLogistic(uc, s, -Pi, Pi);
    else
        dphi = 2 * Pi * uc;

    // Compute _wi_ from sampled hair scattering angles
    Float phi_i = phi_o + dphi;
    Vector3f wi(sinTheta_i, cosTheta_i * std::cos(phi_i), cosTheta_i * std::sin(phi_i));

    // Compute PDF for sampled hair scattering direction _wi_
    Float pdf = 0;
    for (int p = 0; p < pMax; ++p) {
        // Compute $\sin\,\thetao$ and $\cos\,\thetao$ terms accounting for scales
        Float sinThetap_o, cosThetap_o;
        if (p == 0) {
            sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
            cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
        }
        // Handle remainder of $p$ values for hair scale tilt
        else if (p == 1) {
            sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
            cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
        } else if (p == 2) {
            sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
            cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
        } else {
            sinThetap_o = sinTheta_o;
            cosThetap_o = cosTheta_o;
        }

        // Handle out-of-range $\cos\,\thetao$ from scale adjustment
        cosThetap_o = std::abs(cosThetap_o);

        // Handle out-of-range $\cos\,\thetao$ from scale adjustment
        cosThetap_o = std::abs(cosThetap_o);

        pdf += Mp(cosTheta_i, cosThetap_o, sinTheta_i, sinThetap_o, v[p]) * apPDF[p] *
               Np(dphi, p, s, gamma_o, gamma_t);
    }
    pdf += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * apPDF[pMax] *
           (1 / (2 * Pi));

    return BSDFSample(f(wo, wi, mode), wi, pdf, Flags());
}

PBRT_CPU_GPU Float HairBxDF::PDF(Vector3f wo, Vector3f wi, TransportMode mode,
                    BxDFReflTransFlags sampleFlags) const {
    // TODO? flags...

    // Compute hair coordinate system terms related to _wo_
    Float sinTheta_o = wo.x;
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));
    Float phi_o = std::atan2(wo.z, wo.y);
    Float gamma_o = SafeASin(h);

    // Compute hair coordinate system terms related to _wi_
    Float sinTheta_i = wi.x;
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));
    Float phi_i = std::atan2(wi.z, wi.y);

    // Compute $\gammat$ for refracted ray
    Float etap = SafeSqrt(eta * eta - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float gamma_t = SafeASin(sinGamma_t);

    // Compute PDF for $A_p$ terms
    pstd::array<Float, pMax + 1> apPDF = ApPDF(cosTheta_o);

    // Compute PDF sum for hair scattering events
    Float phi = phi_i - phi_o;
    Float pdf = 0;
    for (int p = 0; p < pMax; ++p) {
        // Compute $\sin\,\thetao$ and $\cos\,\thetao$ terms accounting for scales
        Float sinThetap_o, cosThetap_o;
        if (p == 0) {
            sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
            cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
        }
        // Handle remainder of $p$ values for hair scale tilt
        else if (p == 1) {
            sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
            cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
        } else if (p == 2) {
            sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
            cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
        } else {
            sinThetap_o = sinTheta_o;
            cosThetap_o = cosTheta_o;
        }

        // Handle out-of-range $\cos\,\thetao$ from scale adjustment
        cosThetap_o = std::abs(cosThetap_o);

        pdf += Mp(cosTheta_i, cosThetap_o, sinTheta_i, sinThetap_o, v[p]) * apPDF[p] *
               Np(phi, p, s, gamma_o, gamma_t);
    }
    pdf += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * apPDF[pMax] *
           (1 / (2 * Pi));
    return pdf;
}

PBRT_CPU_GPU RGBUnboundedSpectrum HairBxDF::SigmaAFromConcentration(Float ce, Float cp) {
    RGB eumelaninSigma_a(0.419f, 0.697f, 1.37f);
    RGB pheomelaninSigma_a(0.187f, 0.4f, 1.05f);
    RGB sigma_a = ce * eumelaninSigma_a + cp * pheomelaninSigma_a;
#ifdef PBRT_IS_GPU_CODE
    return RGBUnboundedSpectrum(*RGBColorSpace_sRGB, sigma_a);
#else
    return RGBUnboundedSpectrum(*RGBColorSpace::sRGB, sigma_a);
#endif
}

SampledSpectrum HairBxDF::SigmaAFromReflectance(const SampledSpectrum &c, Float beta_n,
                                                const SampledWavelengths &lambda) {
    //髪の反射率から吸収特性 sigma_a を逆算します。髪の反射色から、光の吸収具合を推定するために使用されます。
    SampledSpectrum sigma_a;
    for (int i = 0; i < NSpectrumSamples; ++i)
        sigma_a[i] =
            Sqr(std::log(c[i]) / (5.969f - 0.215f * beta_n + 2.532f * Sqr(beta_n) -
                                  10.73f * Pow<3>(beta_n) + 5.574f * Pow<4>(beta_n) +
                                  0.245f * Pow<5>(beta_n)));
    return sigma_a;
}

std::string HairBxDF::ToString() const {
    return StringPrintf(
        "[ HairBxDF h: %f eta: %f beta_m: %f beta_n: %f v[0]: %f s: %f sigma_a: %s ]", h,
        eta, beta_m, beta_n, v[0], s, sigma_a);
}

//ここから///////////////////////////////////////////////////////////////////////////////////////////////////////
// MorphoBSDF Method Definitions
MorphoBSDF::MorphoBSDF(Float h, Float eta, const SampledSpectrum &sigma_a, 
                       Float beta_m, Float beta_n, Float alpha, int wavelengthIndex)
    : HairBxDF(h, eta, sigma_a, beta_m, beta_n, alpha), wavelengthIndex(wavelengthIndex) {}

SampledSpectrum MorphoBSDF::f(Vector3f wo, Vector3f wi, TransportMode mode) const {
    // wo と wi から角度を計算
    // Compute hair coordinate system terms related to _wo_(_wo_ に関連するヘア座標系の項を計算します)
    Float sinTheta_o = wo.x;
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));
    Float phi_o = std::atan2(wo.z, wo.y);
    Float gamma_o = SafeASin(h);

    // Compute hair coordinate system terms related to _wi_(_wi_ に関連するヘア座標系の項を計算します)
    Float sinTheta_i = wi.x;
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));
    Float phi_i = std::atan2(wi.z, wi.y);

    // Compute $\cos\,\thetat$ for refracted ray(屈折光線の $\cos\,\thetat$ を計算します)
    Float sinTheta_t = sinTheta_o / eta;
    Float cosTheta_t = SafeSqrt(1 - Sqr(sinTheta_t));

    // Compute $\gammat$ for refracted ray(屈折光線の $\gammat$ を計算します)
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float cosGamma_t = SafeSqrt(1 - Sqr(sinGamma_t));
    Float gamma_t = SafeASin(sinGamma_t);

    // 入射角と出射角を用いてテーブル参照を行う
    int it = std::abs(std::round(std::atan2(wi.x, std::sqrt(wi.y * wi.y + wi.z * wi.z)) * 180 / Pi));
    int ot = std::abs(std::round(std::atan2(wo.x, std::sqrt(wo.y * wo.y + wo.z * wo.z)) * 180 / Pi));

    // 散乱光を格納するためのスペクトルを初期化
    SampledSpectrum fsum(0.f);

     // 外部テーブルデータを使用して散乱を計算
    for (int i = 0; i < NSpectrumSamples; ++i) {
        Float brdfValue = CurrentBRDFTable[it][ot][i] / 2.5;
        fsum[i] = brdfValue;
    }

    // Compute the transmittance _T_ of a single path through the cylinder
    SampledSpectrum T = Exp(-sigma_a * (2 * cosGamma_t / cosTheta_t));

    // 最終的な散乱スペクトルを計算
    fsum *= T;

    // `TransportMode` に応じた追加処理
    if (mode == TransportMode::Radiance) {
        // 放射光（Radiance）モード向けの処理が必要ならここに追加
        // 例: fsum *= someFactor;
    }

    // // Compute the transmittance _T_ of a single path through the cylinder(シリンダーを通過する単一パスの透過率 _T_ を計算します)
    // SampledSpectrum T = Exp(-sigma_a * (2 * cosGamma_t / cosTheta_t));

    // // Evaluate hair BSDF(ヘアBSDFを評価する)
    // Float phi = phi_i - phi_o;
    // pstd::array<SampledSpectrum, pMax + 1> ap = Ap(cosTheta_o, eta, h, T);
    // SampledSpectrum fsum(0.);

    // // 多重散乱プロセスをpごとにループ
    // for (int p = 0; p < pMax; ++p) {
    //     // pごとのsinThetaとcosThetaの計算（HairBxDFに準じる）
    //     Float sinThetap_o, cosThetap_o;
    //     if (p == 0) {
    //         sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
    //         cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
    //     } else if (p == 1) {
    //         sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
    //         cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
    //     } else if (p == 2) {
    //         sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
    //         cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
    //     } else {
    //         sinThetap_o = sinTheta_o;
    //         cosThetap_o = cosTheta_o;
    //     }

    //     // 範囲外のcosThetap_oを処理
    //     cosThetap_o = std::abs(cosThetap_o);

    //     int it = std::abs(static_cast<int>(std::round(RadiansToDegrees(std::atan2(wi.x, SafeSqrt(wi.y * wi.y + wi.z * wi.z))))));
    //     int ot = std::abs(static_cast<int>(std::round(RadiansToDegrees(std::atan2(wo.x, SafeSqrt(wo.y * wo.y + wo.z * wo.z))))));

    //     // BRDFテーブルからMpを取得
    //     SampledSpectrum Mp = LookupBRDFTable(it, ot);  // itとotを渡す

    //     // apとNpを使用して散乱寄与を計算し、fsumに加算
    //     fsum += Mp * ap[p] * Np(phi, p, s, gamma_o, gamma_t);  // apとNpの計算はHairBxDFに基づく
    // }

    // // pMax後の残りの寄与を計算
    // fsum += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * ap[pMax] / (2 * Pi);

    // // 入射方向に依存して正規化
    // if (AbsCosTheta(wi) > 0) {
    //     fsum /= AbsCosTheta(wi);
    // }

    return fsum;
}

SampledSpectrum MorphoBSDF::LookupBRDFTable(int it, int ot) const {
    SampledSpectrum reflection(0);
    for (int i = 0; i < NSpectrumSamples; ++i) {  // NSpectrumSamples を使用
        reflection[i] = CurrentBRDFTable[it][ot][i] / 2.5f;
    }
    return reflection;
}

pstd::array<Float, MorphoBSDF::pMax + 1> MorphoBSDF::ComputeApPdf(Float cosTheta_o, const Vector3f &wo) const {
    // _cosTheta_o_ に対応する $A_p$ の配列を計算します
    Float sinTheta_o = SafeSqrt(1 - cosTheta_o * cosTheta_o);

    // 屈折光線の $\cos \thetat$ を計算します
    Float sinTheta_t = sinTheta_o / eta;
    Float cosTheta_t = SafeSqrt(1 - Sqr(sinTheta_t));

    // 屈折光線の $\gammat$ を計算します
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float cosGamma_t = SafeSqrt(1 - Sqr(sinGamma_t));

    // シリンダーを通過する単一パスの透過率 _T_ を計算します
    SampledSpectrum T = Exp(-sigma_a * (2 * cosGamma_t / cosTheta_t));

    // $A_p$ の値を `Ap` 関数を使って計算します
    pstd::array<SampledSpectrum, pMax + 1> ap = Ap(cosTheta_o, eta, h, T);

    // 各 $A_p$ 項から $A_p$ の PDF を計算します
    pstd::array<Float, pMax + 1> apPdf;
    Float sumY = 0;
    for (const SampledSpectrum &as : ap)
        sumY += as.Average();
    for (int i = 0; i <= pMax; ++i)
        apPdf[i] = ap[i].Average() / sumY;

    return apPdf;
}

pstd::optional<BSDFSample> MorphoBSDF::Sample_f(Vector3f wo, Float uc, Point2f u,
                                                TransportMode mode, BxDFReflTransFlags sampleFlags) const {
    // wo に基づいて髪の座標系の項を計算
    Float sinTheta_o = wo.x;
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));
    Float phi_o = std::atan2(wo.z, wo.y);
    Float gamma_o = SafeASin(h);

    // Ap の確率密度関数 (PDF) を計算し、どの p をサンプルするか決定
    pstd::array<Float, pMax + 1> apPdf = ComputeApPdf(cosTheta_o, wo);
    int p = SampleDiscrete(apPdf, uc, nullptr, &uc);

    // スケールを考慮して $\sin \thetao$ と $\cos \thetao$ の項を計算
    Float sinThetap_o, cosThetap_o;
    if (p == 0) {
        sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
        cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
    } else if (p == 1) {
        sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
        cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
    } else if (p == 2) {
        sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
        cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
    } else {
        sinThetap_o = sinTheta_o;
        cosThetap_o = cosTheta_o;
    }

    // テーブルからBRDFを参照するための角度を計算
    int it = std::abs(std::round(std::atan2(wo.x, cosTheta_o) * 180 / Pi));
    int ot = std::abs(std::round(std::atan2(sinThetap_o, cosThetap_o) * 180 / Pi));
    
    // Mpテーブルを参照してサンプル
    SampledSpectrum brdfSample = LookupBRDFTable(it, ot);

    // サンプルされた方向に基づき $\thetai$ を計算
    Float cosTheta = 1 + v[p] * std::log(std::max<Float>(u[0], 1e-5) +
                                         (1 - u[0]) * FastExp(-2 / v[p]));
    Float sinTheta = SafeSqrt(1 - Sqr(cosTheta));
    Float cosPhi = std::cos(2 * Pi * u[1]);
    Float sinTheta_i = -cosTheta * sinThetap_o + sinTheta * cosPhi * cosThetap_o;
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));

    // サンプリングされた方向 $\phi_i$ と $\Delta\phi$ を計算
    Float dphi;
    if (p < pMax)
        dphi = Phi(p, gamma_o, gamma_o) + SampleTrimmedLogistic(uc, s, -Pi, Pi);
    else
        dphi = 2 * Pi * uc;

    // サンプリングされた散乱方向 wi を計算
    Float phi_i = phi_o + dphi;
    Vector3f wi(sinTheta_i, cosTheta_i * std::cos(phi_i), cosTheta_i * std::sin(phi_i));

    // サンプリングされた散乱方向のPDFを計算
    Float pdf = 0;
    for (int p = 0; p < pMax; ++p) {
        Float sinThetap_o, cosThetap_o;
        if (p == 0) {
            sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
            cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
        } else if (p == 1) {
            sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
            cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
        } else if (p == 2) {
            sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
            cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
        } else {
            sinThetap_o = sinTheta_o;
            cosThetap_o = cosTheta_o;
        }
        pdf += Mp(cosTheta_i, cosThetap_o, sinTheta_i, sinThetap_o, v[p]) * apPdf[p] * Np(dphi, p, s, gamma_o, gamma_o);
    }
    pdf += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * apPdf[pMax] * (1 / (2 * Pi));

    // BSDFSample を返す
    return BSDFSample(f(wo, wi, mode), wi, pdf, Flags());
}

Float MorphoBSDF::Pdf(Vector3f wo, Vector3f wi, TransportMode mode,
                      BxDFReflTransFlags sampleFlags) const {
    // 入射方向 wo と出射方向 wi の角度を計算
    Float sinTheta_o = wo.x;
    Float cosTheta_o = SafeSqrt(1 - Sqr(sinTheta_o));
    Float phi_o = std::atan2(wo.z, wo.y);
    Float gamma_o = SafeASin(h);

    Float sinTheta_i = wi.x;
    Float cosTheta_i = SafeSqrt(1 - Sqr(sinTheta_i));
    Float phi_i = std::atan2(wi.z, wi.y);

    // 屈折に関連するパラメータを計算
    Float etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
    Float sinGamma_t = h / etap;
    Float gamma_t = SafeASin(sinGamma_t);

    // Ap の確率密度関数 (PDF) を計算
    pstd::array<Float, pMax + 1> apPDF = ComputeApPdf(cosTheta_o, wo);

    // 散乱イベントに対するPDFを計算
    Float phi = phi_i - phi_o;
    Float pdf = 0.f;

    for (int p = 0; p < pMax; ++p) {
        // pに対応する sinTheta と cosTheta の値を計算
        Float sinThetap_o, cosThetap_o;
        if (p == 0) {
            sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
            cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
        } else if (p == 1) {
            sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
            cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
        } else if (p == 2) {
            sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
            cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
        } else {
            sinThetap_o = sinTheta_o;
            cosThetap_o = cosTheta_o;
        }

        // テーブルからBRDF値を参照
        int it = std::abs(std::round(RadiansToDegrees(std::atan2(wi.x, cosTheta_i))));
        int ot = std::abs(std::round(RadiansToDegrees(std::atan2(wo.x, cosTheta_o))));

        SampledSpectrum Mp = LookupBRDFTable(it, ot);  // it, ot をBRDFテーブル参照に使う

        // Mp と ap を使って PDF を計算
        pdf += Mp.Average() * apPDF[p] * Np(phi, p, s, gamma_o, gamma_t);
    }

    // pMax に対応する項を追加
    pdf += Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) * apPDF[pMax] * (1 / (2 * Pi));

    return pdf;
}
//ここまで//////////////////////////////////////////////////////////


// *****************************************************************************
// Tensor file I/O
// *****************************************************************************

class Tensor {
  public:
    // Data type of the tensor's fields
    enum Type {
        /* Invalid/unspecified */
        Invalid = 0,

        /* Signed and unsigned integer values */
        UInt8,
        Int8,
        UInt16,
        Int16,
        UInt32,
        Int32,
        UInt64,
        Int64,

        /* Floating point values */
        Float16,
        Float32,
        Float64,
    };

    struct Field {
        // Data type of the tensor's fields
        Type dtype;

        // Offset in the file
        size_t offset;

        /// Specifies both rank and size along each dimension
        std::vector<size_t> shape;

        /// Pointer to the start of the tensor
        std::unique_ptr<uint8_t[]> data;
    };

    /// Load a tensor file into memory
    Tensor(const std::string &filename);

    /// Does the file contain a field of the specified name?
    bool has_field(const std::string &name) const;

    /// Return a data structure with information about the specified field
    const Field &field(const std::string &name) const;

    /// Return a human-readable summary
    std::string ToString() const;

    /// Return the total size of the tensor's data
    size_t size() const { return m_size; }

    std::string filename() const { return m_filename; }

  private:
    std::unordered_map<std::string, Field> m_fields;
    std::string m_filename;
    size_t m_size;
};

static std::ostream &operator<<(std::ostream &os, Tensor::Type value) {
    switch (value) {
    case Tensor::Invalid:
        os << "invalid";
        break;
    case Tensor::UInt8:
        os << "uint8_t";
        break;
    case Tensor::Int8:
        os << "int8_t";
        break;
    case Tensor::UInt16:
        os << "uint16_t";
        break;
    case Tensor::Int16:
        os << "int16_t";
        break;
    case Tensor::UInt32:
        os << "uint32_t";
        break;
    case Tensor::Int32:
        os << "int8_t";
        break;
    case Tensor::UInt64:
        os << "uint64_t";
        break;
    case Tensor::Int64:
        os << "int64_t";
        break;
    case Tensor::Float16:
        os << "float16_t";
        break;
    case Tensor::Float32:
        os << "float32_t";
        break;
    case Tensor::Float64:
        os << "float64_t";
        break;
    default:
        os << "unknown";
        break;
    }
    return os;
}

static size_t type_size(Tensor::Type value) {
    switch (value) {
    case Tensor::Invalid:
        return 0;
        break;
    case Tensor::UInt8:
        return 1;
        break;
    case Tensor::Int8:
        return 1;
        break;
    case Tensor::UInt16:
        return 2;
        break;
    case Tensor::Int16:
        return 2;
        break;
    case Tensor::UInt32:
        return 4;
        break;
    case Tensor::Int32:
        return 4;
        break;
    case Tensor::UInt64:
        return 8;
        break;
    case Tensor::Int64:
        return 8;
        break;
    case Tensor::Float16:
        return 2;
        break;
    case Tensor::Float32:
        return 4;
        break;
    case Tensor::Float64:
        return 8;
        break;
    default:
        return 0;
        break;
    }
}

Tensor::Tensor(const std::string &filename) : m_filename(filename) {
    // Helpful macros to limit error-handling code duplication
#ifdef ASSERT
#undef ASSERT
#endif  // ASSERT

#define ASSERT(cond, msg)                            \
    do {                                             \
        if (!(cond)) {                               \
            fclose(file);                            \
            ErrorExit("%s: Tensor: " msg, filename); \
        }                                            \
    } while (0)

#define SAFE_READ(vars, size, count) \
    ASSERT(fread(vars, size, count, file) == (count), "Unable to read " #vars ".")

    FILE *file = FOpenRead(filename);
    if (file == NULL)
        ErrorExit("%s: unable to open file", filename);

    ASSERT(!fseek(file, 0, SEEK_END), "Unable to seek to end of file.");

    long size = ftell(file);
    ASSERT(size != -1, "Unable to tell file cursor position.");
    m_size = static_cast<size_t>(size);
    rewind(file);

    ASSERT(m_size >= 12 + 2 + 4, "Invalid tensor file: too small, truncated?");

    uint8_t header[12], version[2];
    uint32_t n_fields;
    SAFE_READ(header, sizeof(*header), 12);
    SAFE_READ(version, sizeof(*version), 2);
    SAFE_READ(&n_fields, sizeof(n_fields), 1);

    ASSERT(memcmp(header, "tensor_file", 12) == 0,
           "Invalid tensor file: invalid header.");
    ASSERT(version[0] == 1 && version[1] == 0,
           "Invalid tensor file: unknown file version.");

    for (uint32_t i = 0; i < n_fields; ++i) {
        uint8_t dtype;
        uint16_t name_length, ndim;
        uint64_t offset;

        SAFE_READ(&name_length, sizeof(name_length), 1);
        std::string name(name_length, '\0');
        SAFE_READ((char *)name.data(), 1, name_length);
        SAFE_READ(&ndim, sizeof(ndim), 1);
        SAFE_READ(&dtype, sizeof(dtype), 1);
        SAFE_READ(&offset, sizeof(offset), 1);
        ASSERT(dtype != Invalid && dtype <= Float64,
               "Invalid tensor file: unknown type.");

        std::vector<size_t> shape(ndim);
        size_t total_size = type_size((Type)dtype);  // no need to check here, line 43
                                                     // already removes invalid types
        for (size_t j = 0; j < (size_t)ndim; ++j) {
            uint64_t size_value;
            SAFE_READ(&size_value, sizeof(size_value), 1);
            shape[j] = (size_t)size_value;
            total_size *= shape[j];
        }

        auto data = std::unique_ptr<uint8_t[]>(new uint8_t[total_size]);

        long cur_pos = ftell(file);
        ASSERT(cur_pos != -1, "Unable to tell current cursor position.");
        ASSERT(fseek(file, offset, SEEK_SET) != -1, "Unable to seek to tensor offset.");
        SAFE_READ(data.get(), 1, total_size);
        ASSERT(fseek(file, cur_pos, SEEK_SET) != -1,
               "Unable to seek back to current position");

        m_fields[name] =
            Field{(Type)dtype, static_cast<size_t>(offset), shape, std::move(data)};
    }

    fclose(file);

#undef SAFE_READ
#undef ASSERT
}

/// Does the file contain a field of the specified name?
bool Tensor::has_field(const std::string &name) const {
    return m_fields.find(name) != m_fields.end();
}

/// Return a data structure with information about the specified field
const Tensor::Field &Tensor::field(const std::string &name) const {
    auto it = m_fields.find(name);
    CHECK(it != m_fields.end());
    return it->second;
}

/// Return a human-readable summary
std::string Tensor::ToString() const {
    std::ostringstream oss;
    oss << "Tensor[" << std::endl
        << "  filename = \"" << m_filename << "\"," << std::endl
        << "  size = " << size() << "," << std::endl
        << "  fields = {" << std::endl;

    size_t ctr = 0;
    for (const auto &it : m_fields) {
        oss << "    \"" << it.first << "\""
            << " => [" << std::endl
            << "      dtype = " << it.second.dtype << "," << std::endl
            << "      offset = " << it.second.offset << "," << std::endl
            << "      shape = [";
        const auto &shape = it.second.shape;
        for (size_t j = 0; j < shape.size(); ++j) {
            oss << shape[j];
            if (j + 1 < shape.size())
                oss << ", ";
        }

        oss << "]" << std::endl;

        oss << "    ]";
        if (++ctr < m_fields.size())
            oss << ",";
        oss << std::endl;
    }

    oss << "  }" << std::endl << "]";

    return oss.str();
}

// MeasuredBxDFData Definition
struct MeasuredBxDFData {
    // MeasuredBxDFData Public Members
    pstd::vector<float> wavelengths;
    PiecewiseLinear2D<3> spectra;
    PiecewiseLinear2D<0> ndf;
    PiecewiseLinear2D<2> vndf;
    PiecewiseLinear2D<0> sigma;
    bool isotropic;
    PiecewiseLinear2D<2> luminance;
    MeasuredBxDFData(Allocator alloc)
        : ndf(alloc),
          sigma(alloc),
          vndf(alloc),
          luminance(alloc),
          spectra(alloc),
          wavelengths(alloc) {}

    static MeasuredBxDFData *Create(const std::string &filename, Allocator alloc);

    std::string ToString() const {
        return StringPrintf("[ MeasuredBxDFData filename: %s ]", filename);
    }

    std::string filename;
};

STAT_MEMORY_COUNTER("Memory/Measured BRDF data", measuredBRDFBytes);

MeasuredBxDFData *MeasuredBxDFData::Create(const std::string &filename, Allocator alloc) {
    Tensor tf = Tensor(filename);
    auto &theta_i = tf.field("theta_i");
    auto &phi_i = tf.field("phi_i");
    auto &ndf = tf.field("ndf");
    auto &sigma = tf.field("sigma");
    auto &vndf = tf.field("vndf");
    auto &spectra = tf.field("spectra");
    auto &luminance = tf.field("luminance");
    auto &wavelengths = tf.field("wavelengths");
    auto &description = tf.field("description");
    auto &jacobian = tf.field("jacobian");

    if (!(description.shape.size() == 1 && description.dtype == Tensor::UInt8 &&

          theta_i.shape.size() == 1 && theta_i.dtype == Tensor::Float32 &&

          phi_i.shape.size() == 1 && phi_i.dtype == Tensor::Float32 &&

          wavelengths.shape.size() == 1 && wavelengths.dtype == Tensor::Float32 &&

          ndf.shape.size() == 2 && ndf.dtype == Tensor::Float32 &&

          sigma.shape.size() == 2 && sigma.dtype == Tensor::Float32 &&

          vndf.shape.size() == 4 && vndf.dtype == Tensor::Float32 &&
          vndf.shape[0] == phi_i.shape[0] && vndf.shape[1] == theta_i.shape[0] &&

          luminance.shape.size() == 4 && luminance.dtype == Tensor::Float32 &&
          luminance.shape[0] == phi_i.shape[0] &&
          luminance.shape[1] == theta_i.shape[0] &&
          luminance.shape[2] == luminance.shape[3] &&

          spectra.dtype == Tensor::Float32 && spectra.shape.size() == 5 &&
          spectra.shape[0] == phi_i.shape[0] && spectra.shape[1] == theta_i.shape[0] &&
          spectra.shape[2] == wavelengths.shape[0] &&
          spectra.shape[3] == spectra.shape[4] &&

          luminance.shape[2] == spectra.shape[3] &&
          luminance.shape[3] == spectra.shape[4] &&

          jacobian.shape.size() == 1 && jacobian.shape[0] == 1 &&
          jacobian.dtype == Tensor::UInt8)) {
        Error("%s: invalid BRDF file structure: %s", filename, tf);
        return nullptr;
    }

    MeasuredBxDFData *brdf = alloc.new_object<MeasuredBxDFData>(alloc);
    brdf->filename = filename;
    brdf->isotropic = phi_i.shape[0] <= 2;

    if (!brdf->isotropic) {
        float *phi_i_data = (float *)phi_i.data.get();
        int reduction =
            (int)std::rint((2 * Pi) / (phi_i_data[phi_i.shape[0] - 1] - phi_i_data[0]));
        if (reduction != 1)
            ErrorExit("%s: reduction %d (!= 1) not supported", filename, reduction);
    }

    /* Construct NDF interpolant data structure */
    brdf->ndf = PiecewiseLinear2D<0>(alloc, (float *)ndf.data.get(), ndf.shape[1],
                                     ndf.shape[0], {}, {}, false, false);

    /* Construct projected surface area interpolant data structure */
    brdf->sigma = PiecewiseLinear2D<0>(alloc, (float *)sigma.data.get(), sigma.shape[1],
                                       sigma.shape[0], {}, {}, false, false);

    /* Construct VNDF warp data structure */
    brdf->vndf = PiecewiseLinear2D<2>(
        alloc, (float *)vndf.data.get(), vndf.shape[3], vndf.shape[2],
        {{(int)phi_i.shape[0], (int)theta_i.shape[0]}},
        {{(const float *)phi_i.data.get(), (const float *)theta_i.data.get()}});

    /* Construct Luminance warp data structure */
    brdf->luminance = PiecewiseLinear2D<2>(
        alloc, (float *)luminance.data.get(), luminance.shape[3], luminance.shape[2],
        {{(int)phi_i.shape[0], (int)theta_i.shape[0]}},
        {{(const float *)phi_i.data.get(), (const float *)theta_i.data.get()}});

    /* Copy wavelength information */
    size_t size = wavelengths.shape[0];
    brdf->wavelengths.resize(size);
    for (size_t i = 0; i < size; ++i)
        brdf->wavelengths[i] = ((const float *)wavelengths.data.get())[i];

    /* Construct spectral interpolant */
    brdf->spectra = PiecewiseLinear2D<3>(
        alloc, (float *)spectra.data.get(), spectra.shape[4], spectra.shape[3],
        {{(int)phi_i.shape[0], (int)theta_i.shape[0], (int)wavelengths.shape[0]}},
        {{(const float *)phi_i.data.get(), (const float *)theta_i.data.get(),
          (const float *)wavelengths.data.get()}},
        false, false);

    measuredBRDFBytes += sizeof(MeasuredBxDFData) + 4 * brdf->wavelengths.size() +
                         brdf->ndf.BytesUsed() + brdf->sigma.BytesUsed() +
                         brdf->vndf.BytesUsed() + brdf->luminance.BytesUsed() +
                         brdf->spectra.BytesUsed();

    return brdf;
}

MeasuredBxDFData *MeasuredBxDF::BRDFDataFromFile(const std::string &filename,
                                                 Allocator alloc) {
    static std::map<std::string, MeasuredBxDFData *> loadedData;
    if (loadedData.find(filename) == loadedData.end())
        loadedData[filename] = MeasuredBxDFData::Create(filename, alloc);
    return loadedData[filename];
}

// MeasuredBxDF Method Definitions
SampledSpectrum MeasuredBxDF::f(Vector3f wo, Vector3f wi, TransportMode mode) const {
    // Check for valid reflection configurations
    if (!SameHemisphere(wo, wi))
        return SampledSpectrum(0);
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }

    // Determine half-direction vector $\wm$
    Vector3f wm = wi + wo;
    if (LengthSquared(wm) == 0)
        return SampledSpectrum(0);
    wm = Normalize(wm);

    // Map $\wo$ and $\wm$ to the unit square $[0,\,1]^2$
    Float theta_o = SphericalTheta(wo), phi_o = std::atan2(wo.y, wo.x);
    Float theta_m = SphericalTheta(wm), phi_m = std::atan2(wm.y, wm.x);
    Point2f u_wo(theta2u(theta_o), phi2u(phi_o));
    Point2f u_wm(theta2u(theta_m), phi2u(brdf->isotropic ? (phi_m - phi_o) : phi_m));
    u_wm[1] = u_wm[1] - pstd::floor(u_wm[1]);

    // Evaluate inverse parameterization $R^{-1}$
    PLSample ui = brdf->vndf.Invert(u_wm, phi_o, theta_o);

    // Evaluate spectral 5D interpolant
    SampledSpectrum fr;
    for (int i = 0; i < NSpectrumSamples; ++i)
        fr[i] =
            std::max<Float>(0, brdf->spectra.Evaluate(ui.p, phi_o, theta_o, lambda[i]));

    // Return measured BRDF value
    return fr * brdf->ndf.Evaluate(u_wm) /
           (4 * brdf->sigma.Evaluate(u_wo) * CosTheta(wi));
}

pstd::optional<BSDFSample> MeasuredBxDF::Sample_f(Vector3f wo, Float uc, Point2f u,
                                                  TransportMode mode,
                                                  BxDFReflTransFlags sampleFlags) const {
    // Check flags and detect interactions in lower hemisphere
    if (!(sampleFlags & BxDFReflTransFlags::Reflection))
        return {};
    bool flipWi = false;
    if (wo.z <= 0) {
        wo = -wo;
        flipWi = true;
    }

    // Initialize parameters of conditional distribution
    Float theta_o = SphericalTheta(wo), phi_o = std::atan2(wo.y, wo.x);

    // Warp sample using luminance distribution
    auto s = brdf->luminance.Sample(u, phi_o, theta_o);
    u = s.p;
    Float lum_pdf = s.pdf;

    // Sample visible normal distribution of measured BRDF
    s = brdf->vndf.Sample(u, phi_o, theta_o);
    Point2f u_wm = s.p;
    Float pdf = s.pdf;

    // Map from microfacet normal to incident direction
    Float phi_m = u2phi(u_wm.y), theta_m = u2theta(u_wm.x);
    if (brdf->isotropic)
        phi_m += phi_o;
    Float sinTheta_m = std::sin(theta_m), cosTheta_m = std::cos(theta_m);
    Vector3f wm = SphericalDirection(sinTheta_m, cosTheta_m, phi_m);
    Vector3f wi = Reflect(wo, wm);
    if (wi.z <= 0)
        return {};

    // Interpolate spectral BRDF
    SampledSpectrum fr(0);
    for (int i = 0; i < NSpectrumSamples; ++i)
        fr[i] = std::max<Float>(0, brdf->spectra.Evaluate(u, phi_o, theta_o, lambda[i]));

    Point2f u_wo(theta2u(theta_o), phi2u(phi_o));
    fr *= brdf->ndf.Evaluate(u_wm) / (4 * brdf->sigma.Evaluate(u_wo) * AbsCosTheta(wi));
    pdf /= 4 * Dot(wo, wm) * std::max<Float>(2 * Sqr(Pi) * u_wm.x * sinTheta_m, 1e-6f);

    // Handle interactions in lower hemisphere
    if (flipWi)
        wi = -wi;

    return BSDFSample(fr, wi, pdf * lum_pdf, BxDFFlags::GlossyReflection);
}

Float MeasuredBxDF::PDF(Vector3f wo, Vector3f wi, TransportMode mode,
                        BxDFReflTransFlags sampleFlags) const {
    if (!(sampleFlags & BxDFReflTransFlags::Reflection))
        return 0;
    if (!SameHemisphere(wo, wi))
        return 0;
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }

    Vector3f wm = wi + wo;
    if (LengthSquared(wm) == 0)
        return 0;
    wm = Normalize(wm);

    /* Cartesian -> spherical coordinates */
    Float theta_o = SphericalTheta(wo), phi_o = std::atan2(wo.y, wo.x);
    Float theta_m = SphericalTheta(wm), phi_m = std::atan2(wm.y, wm.x);

    /* Spherical coordinates -> unit coordinate system */
    Point2f u_wm(theta2u(theta_m), phi2u(brdf->isotropic ? (phi_m - phi_o) : phi_m));
    u_wm.y = u_wm.y - pstd::floor(u_wm.y);

    auto ui = brdf->vndf.Invert(u_wm, phi_o, theta_o);
    Point2f sample = ui.p;
    Float vndfPDF = ui.pdf;

    Float pdf = brdf->luminance.Evaluate(sample, phi_o, theta_o);
    Float sinTheta_m = std::sqrt(Sqr(wm.x) + Sqr(wm.y));
    Float jacobian =
        4.f * Dot(wo, wm) * std::max<Float>(2 * Sqr(Pi) * u_wm.x * sinTheta_m, 1e-6f);
    return vndfPDF * pdf / jacobian;
}

std::string MeasuredBxDF::ToString() const {
    return StringPrintf("[ MeasuredBxDF brdf: %s ]", *brdf);
}

std::string NormalizedFresnelBxDF::ToString() const {
    return StringPrintf("[ NormalizedFresnelBxDF eta: %f ]", eta);
}

// BxDF Method Definitions
SampledSpectrum BxDF::rho(Vector3f wo, pstd::span<const Float> uc,
                          pstd::span<const Point2f> u2) const {
    if (wo.z == 0)
        return {};
    SampledSpectrum r(0.);
    DCHECK_EQ(uc.size(), u2.size());
    for (size_t i = 0; i < uc.size(); ++i) {
        // Compute estimate of $\rho_\roman{hd}$
        pstd::optional<BSDFSample> bs = Sample_f(wo, uc[i], u2[i]);
        if (bs && bs->pdf > 0)
            r += bs->f * AbsCosTheta(bs->wi) / bs->pdf;
    }
    return r / uc.size();
}

SampledSpectrum BxDF::rho(pstd::span<const Point2f> u1, pstd::span<const Float> uc,
                          pstd::span<const Point2f> u2) const {
    DCHECK_EQ(uc.size(), u1.size());
    DCHECK_EQ(u1.size(), u2.size());
    SampledSpectrum r(0.f);
    for (size_t i = 0; i < uc.size(); ++i) {
        // Compute estimate of $\rho_\roman{hh}$
        Vector3f wo = SampleUniformHemisphere(u1[i]);
        if (wo.z == 0)
            continue;
        Float pdfo = UniformHemispherePDF();
        pstd::optional<BSDFSample> bs = Sample_f(wo, uc[i], u2[i]);
        if (bs && bs->pdf > 0)
            r += bs->f * AbsCosTheta(bs->wi) * AbsCosTheta(wo) / (pdfo * bs->pdf);
    }
    return r / (Pi * uc.size());
}

std::string BxDF::ToString() const {
    auto toStr = [](auto ptr) { return ptr->ToString(); };
    return DispatchCPU(toStr);
}

template class LayeredBxDF<DielectricBxDF, DiffuseBxDF, true>;
template class LayeredBxDF<DielectricBxDF, ConductorBxDF, true>;

}  // namespace pbrt
