// license:GPLv3+

#pragma once


class InputFilter
{
public:
   virtual ~InputFilter() { }
   virtual void Push(uint64_t timestampNs, float value) = 0;
   virtual float Get() = 0;
};


class NoOpInputFilter final : public InputFilter
{
public:
   ~NoOpInputFilter() { }
   void Push(uint64_t timestampNs, float value) override { m_value = value; }
   float Get() override { return m_value; }

private:
   float m_value = 0.f;
};


class BasicFirstDerivative final : public InputFilter
{
public:
   ~BasicFirstDerivative() { }
   void Push(uint64_t timestampNs, float value) override;
   float Get() override { return m_value; }

private:
   bool m_initialized = false;
   uint64_t m_t0, m_t1;
   float m_p0, m_p1;
   float m_value = 0.f;
};


class BasicSecondDerivative final : public InputFilter
{
public:
   ~BasicSecondDerivative() { }
   void Push(uint64_t timestampNs, float value) override;
   float Get() override { return m_value; }

private:
   bool m_initialized = false;
   uint64_t m_t0, m_t1, m_t2;
   float m_p0, m_p1, m_p2;
   float m_value = 0.f;
};


// A simple Butterworth IIR filter with a 10Hz passband for filtered plunger position
// This filters is lazily advanced when value is requested, using overall game time
class PlungerPositionFilter final : public InputFilter
{
public:
   ~PlungerPositionFilter() { }
   void Push(uint64_t timestampNs, float value) override { AdvanceFilter(); m_sensorValue = value; }
   float Get() override { AdvanceFilter(); return m_value; }

private:
   void AdvanceFilter();

   static constexpr const int IIR_Order = 4;

   // coefficients for IIR_Order Butterworth filter set to 10 Hz passband
   static constexpr float IIR_a[IIR_Order + 1] = {
      0.0048243445f,
      0.019297378f,
      0.028946068f,
      0.019297378f,
      0.0048243445f };

   static constexpr float IIR_b[IIR_Order + 1] = {
      1.00000000f, //if not 1 add division below
      -2.369513f,
      2.3139884f,
      -1.0546654f,
      0.1873795f };

   int m_init = IIR_Order;
   float m_x[IIR_Order + 1] = {};
   float m_y[IIR_Order + 1] = {};

   uint32_t m_filterTimestampMs = 0;
   float m_sensorValue = 0.f;
   float m_value = 0.f;
};


// A nudge acceleration filter to suppress low frequency noise
// This filters is lazily advanced when value is requested, using overall game time
class NudgeAccelerationFilter final : public InputFilter
{
public:
   ~NudgeAccelerationFilter() { }
   void Push(uint64_t timestampNs, float value) override
   {
      AdvanceFilter();
      m_sensorValue = value;
   }
   float Get() override
   {
      AdvanceFilter();
      return m_value;
   }

private:
   void AdvanceFilter();

   // running total of samples
   float m_sum = 0.f;

   // previous sample
   float m_prv = 0.f;

   // timestamp of last zero crossing in the raw acceleration data
   uint64_t m_tzc = 0;

   // timestamp of last correction inserted into the data
   uint64_t m_tCorr = 0;

   // timestamp of last motion == start of rest
   uint64_t m_tMotion = 0;

   uint32_t m_filterTimestampMs = 0;
   float m_sensorValue = 0.f;
   float m_value = 0.f;
};