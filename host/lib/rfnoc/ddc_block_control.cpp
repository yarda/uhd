//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc/ddc_block_control.hpp>
#include <uhd/rfnoc/property.hpp>
#include <uhd/rfnoc/registry.hpp>
#include <uhd/rfnoc/defaults.hpp>
#include <uhd/types/ranges.hpp>
#include <uhd/utils/log.hpp>
#include <uhdlib/usrp/cores/dsp_core_utils.hpp>
#include <uhdlib/utils/compat_check.hpp>
#include <uhdlib/utils/math.hpp>
#include <boost/math/special_functions/round.hpp>
#include <set>
#include <string>

namespace {

constexpr double DEFAULT_RATE    = 1e9;
constexpr double DEFAULT_SCALING = 1.0;
constexpr int DEFAULT_DECIM      = 1;
constexpr double DEFAULT_FREQ    = 0.0;
const uhd::rfnoc::io_type_t DEFAULT_TYPE     = uhd::rfnoc::IO_TYPE_SC16;

//! Space (in bytes) between register banks per channel
constexpr uint32_t REG_CHAN_OFFSET = 2048;

} // namespace

using namespace uhd::rfnoc;

const uint16_t ddc_block_control::MINOR_COMPAT = 0;
const uint16_t ddc_block_control::MAJOR_COMPAT = 0;

const uint32_t ddc_block_control::RB_COMPAT_NUM    = 0; // read this first
const uint32_t ddc_block_control::RB_NUM_HB        = 8;
const uint32_t ddc_block_control::RB_CIC_MAX_DECIM = 16;

const uint32_t ddc_block_control::SR_N_ADDR        = 128 * 8;
const uint32_t ddc_block_control::SR_M_ADDR        = 129 * 8;
const uint32_t ddc_block_control::SR_CONFIG_ADDR   = 130 * 8;
const uint32_t ddc_block_control::SR_FREQ_ADDR     = 132 * 8;
const uint32_t ddc_block_control::SR_SCALE_IQ_ADDR = 133 * 8;
const uint32_t ddc_block_control::SR_DECIM_ADDR    = 134 * 8;
const uint32_t ddc_block_control::SR_MUX_ADDR      = 135 * 8;
const uint32_t ddc_block_control::SR_COEFFS_ADDR   = 136 * 8;

class ddc_block_control_impl : public ddc_block_control
{
public:
    RFNOC_BLOCK_CONSTRUCTOR(ddc_block_control)
    , _fpga_compat(regs().peek32(RB_COMPAT_NUM)),
        _num_halfbands(regs().peek32(RB_NUM_HB)),
        _cic_max_decim(regs().peek32(RB_CIC_MAX_DECIM)),
        _residual_scaling(get_num_input_ports(), DEFAULT_SCALING)
    {
        UHD_ASSERT_THROW(get_num_input_ports() == get_num_output_ports());
        UHD_ASSERT_THROW(_cic_max_decim > 0 && _cic_max_decim <= 0xFF);
        uhd::assert_fpga_compat(MAJOR_COMPAT,
            MINOR_COMPAT,
            _fpga_compat,
            get_unique_id(),
            get_unique_id(),
            false /* Let it slide if minors mismatch */
        );
        RFNOC_LOG_DEBUG("Loading DDC with " << _num_halfbands
                                            << " halfbands and "
                                               "max CIC decimation "
                                            << _cic_max_decim);
        // Load list of valid decimation values
        std::set<size_t> decims{1}; // 1 is always a valid decimatino
        for (size_t hb = 0; hb < _num_halfbands; hb++) {
            for (size_t cic_decim = 0; cic_decim < _cic_max_decim; cic_decim++) {
                decims.insert((1 << hb) * cic_decim);
            }
        }
        for (size_t decim : decims) {
            _valid_decims.push_back(uhd::range_t(double(decim)));
        }

        // Initialize properties. It is very important to first reserve the
        // space, because we use push_back() further down, and properties must
        // not change their base address after registration and resolver
        // creation.
        _samp_rate_in.reserve(get_num_ports());
        _samp_rate_out.reserve(get_num_ports());
        _scaling_in.reserve(get_num_ports());
        _scaling_out.reserve(get_num_ports());
        _decim.reserve(get_num_ports());
        _freq.reserve(get_num_ports());
        _type_in.reserve(get_num_ports());
        _type_out.reserve(get_num_ports());
        for (size_t chan = 0; chan < get_num_ports(); chan++) {
            _register_props(chan);
        }
        register_issue_stream_cmd();
    }

    double set_freq(const double freq,
        const size_t chan,
        const boost::optional<uhd::time_spec_t> time)
    {
        // Store the current command time so we can restore it later
        auto prev_cmd_time = get_command_time(chan);
        if (time) {
            set_command_time(time.get(), chan);
        }
        // This will trigger property propagation:
        set_property<double>("freq", freq, chan);
        set_command_time(prev_cmd_time, chan);
        return get_freq(chan);
    }

    double get_freq(const size_t chan) const
    {
        return _freq.at(chan).get();
    }

    uhd::freq_range_t get_frequency_range(const size_t chan) const
    {
        const double input_rate = _samp_rate_in.at(chan).get();
        // TODO add steps
        return uhd::freq_range_t(-input_rate / 2, input_rate / 2);
    }

    double get_input_rate(const size_t chan) const
    {
        return _samp_rate_in.at(chan).get();
    }

    double get_output_rate(const size_t chan) const
    {
        return _samp_rate_out.at(chan).get();
    }

    uhd::meta_range_t get_output_rates(const size_t chan) const
    {
        uhd::meta_range_t result;
        const double input_rate = _samp_rate_in.at(chan).get();
        // The decimations are stored in order (from smallest to biggest), so
        // iterate in reverse order so we can add rates from smallest to biggest
        for (auto it = _valid_decims.rbegin(); it != _valid_decims.rend(); ++it) {
            result.push_back(uhd::range_t(input_rate / it->start()));
        }
        return result;
    }

    double set_output_rate(const double rate, const size_t chan)
    {
        const int coerced_decim = coerce_decim(get_input_rate(chan) / rate);
        set_property<int>("decim", coerced_decim, chan);
        return _decim.at(chan).get();
    }

    // Somewhat counter-intuitively, we post a stream command as a message to
    // ourselves. That's because it's easier to re-use the message handler than
    // it is to reuse the issue_stream_cmd() API call, because this API call
    // will always be forwarded to the upstream block, whereas the message
    // handler goes both ways.
    // This way, calling issue_stream_cmd() is the same as posting a message to
    // our output port.
    void issue_stream_cmd(const uhd::stream_cmd_t& stream_cmd, const size_t port)
    {
        RFNOC_LOG_TRACE("issue_stream_cmd(stream_mode="
                        << char(stream_cmd.stream_mode) << ", port=" << port);
        res_source_info dst_edge{res_source_info::OUTPUT_EDGE, port};
        auto new_action = stream_cmd_action_info::make(stream_cmd.stream_mode);
        new_action->stream_cmd = stream_cmd;
        issue_stream_cmd_action_handler(dst_edge, new_action);
    }

private:
    //! Shorthand for num ports, since num input ports always equals num output ports
    inline size_t get_num_ports()
    {
        return get_num_input_ports();
    }

    inline uint32_t get_addr(const uint32_t base_addr, const size_t chan)
    {
        return base_addr + REG_CHAN_OFFSET * chan;
    }

    /**************************************************************************
     * Initialization
     *************************************************************************/
    void _register_props(const size_t chan)
    {
        // Create actual properties and store them
        _samp_rate_in.push_back(property_t<double>(
            PROP_KEY_SAMP_RATE, DEFAULT_RATE, {res_source_info::INPUT_EDGE, chan}));
        _samp_rate_out.push_back(property_t<double>(
            PROP_KEY_SAMP_RATE, DEFAULT_RATE, {res_source_info::OUTPUT_EDGE, chan}));
        _scaling_in.push_back(property_t<double>(
            PROP_KEY_SCALING, DEFAULT_SCALING, {res_source_info::INPUT_EDGE, chan}));
        _scaling_out.push_back(property_t<double>(
            PROP_KEY_SCALING, DEFAULT_SCALING, {res_source_info::OUTPUT_EDGE, chan}));
        _decim.push_back(property_t<int>(
            PROP_KEY_DECIM, DEFAULT_DECIM, {res_source_info::USER, chan}));
        _freq.push_back(property_t<double>(
            PROP_KEY_FREQ, DEFAULT_FREQ, {res_source_info::USER, chan}));
        _type_in.emplace_back(property_t<std::string>(
            PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::INPUT_EDGE, chan}));
        _type_out.emplace_back(property_t<std::string>(
            PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::OUTPUT_EDGE, chan}));
        UHD_ASSERT_THROW(_samp_rate_in.size() == chan + 1);
        UHD_ASSERT_THROW(_samp_rate_out.size() == chan + 1);
        UHD_ASSERT_THROW(_scaling_in.size() == chan + 1);
        UHD_ASSERT_THROW(_scaling_out.size() == chan + 1);
        UHD_ASSERT_THROW(_decim.size() == chan + 1);
        UHD_ASSERT_THROW(_freq.size() == chan + 1);
        UHD_ASSERT_THROW(_type_in.size() == chan + 1);
        UHD_ASSERT_THROW(_type_out.size() == chan + 1);

        // give us some shorthands for the rest of this function
        property_t<double>* samp_rate_in  = &_samp_rate_in.back();
        property_t<double>* samp_rate_out = &_samp_rate_out.back();
        property_t<double>* scaling_in    = &_scaling_in.back();
        property_t<double>* scaling_out   = &_scaling_out.back();
        property_t<int>* decim            = &_decim.back();
        property_t<double>* freq          = &_freq.back();
        property_t<std::string>* type_in  = &_type_in.back();
        property_t<std::string>* type_out = &_type_out.back();

        // register them
        register_property(samp_rate_in);
        register_property(samp_rate_out);
        register_property(scaling_in);
        register_property(scaling_out);
        register_property(decim);
        register_property(freq);
        register_property(type_in);
        register_property(type_out);

        /**********************************************************************
         * Add resolvers
         *********************************************************************/
        // Resolver for the output scaling: This cannot be updated, we reset it
        // to its previous value.
        add_property_resolver({scaling_out},
            {scaling_out},
            [this,
                chan,
                &decim       = *decim,
                &scaling_in  = *scaling_in,
                &scaling_out = *scaling_out]() {
                scaling_out = scaling_in.get() * _residual_scaling.at(chan);
            });
        // Resolver for _decim: this gets executed when the user directly
        // modifies _decim. the desired behaviour is to coerce it first, then
        // keep the input rate constant, and re-calculate the output rate.
        add_property_resolver({decim},
            {decim, samp_rate_out, scaling_in},
            [this,
                chan,
                &decim         = *decim,
                &samp_rate_out = *samp_rate_out,
                &samp_rate_in  = *samp_rate_in,
                &scaling_in    = *scaling_in]() {
                RFNOC_LOG_TRACE("Calling resolver for `decim'@" << chan);
                decim = coerce_decim(double(decim.get()));
                set_decim(decim.get(), chan);
                samp_rate_out = samp_rate_in.get() / decim.get();
                scaling_in.force_dirty();
            });
        // Resolver for _freq: this gets executed when the user directly
        // modifies _freq.
        add_property_resolver(
            {freq}, {freq}, [this, chan, &samp_rate_in = *samp_rate_in, &freq = *freq]() {
                RFNOC_LOG_TRACE("Calling resolver for `freq'@" << chan);
                freq = _set_freq(freq.get(), samp_rate_in.get(), chan);
            });
        // Resolver for the input rate: we try and match decim so that the output
        // rate is not modified. if decim needs to be coerced, only then the
        // output rate is modified.
        // Note this will also affect the frequency.
        add_property_resolver({samp_rate_in},
            {decim, samp_rate_out, scaling_in, freq},
            [this,
                chan,
                &decim         = *decim,
                &freq          = *freq,
                &scaling_in    = *scaling_in,
                &samp_rate_out = *samp_rate_out,
                &samp_rate_in  = *samp_rate_in]() {
                RFNOC_LOG_TRACE("Calling resolver for `samp_rate_in'@" << chan);
                // If decim changes, it will trigger the decim resolver to run
                decim         = coerce_decim(samp_rate_in.get() / samp_rate_out.get());
                samp_rate_out = samp_rate_in.get() / decim.get();
                // If the input rate changes, we need to update the DDS, too,
                // since it works on frequencies normalized by the input rate.
                freq.force_dirty();
            });
        // Resolver for the output rate: like the previous one, but flipped.
        add_property_resolver({samp_rate_out},
            {decim, samp_rate_in},
            [this,
                chan,
                &decim         = *decim,
                &scaling_in    = *scaling_in,
                &samp_rate_out = *samp_rate_out,
                &samp_rate_in  = *samp_rate_in]() {
                RFNOC_LOG_TRACE("Calling resolver for `samp_rate_out'@" << chan);
                decim = coerce_decim(int(samp_rate_in.get() / samp_rate_out.get()));
                // If decim is dirty, it will trigger the decim resolver.
                // However, the decim resolver will set the output rate based
                // on the input rate, so we need to force the input rate first.
                if (decim.is_dirty()) {
                    samp_rate_in = samp_rate_out.get() * decim.get();
                }
            });
        // Resolver for the input scaling: When updated, we forward the changes
        // to the output scaling.
        add_property_resolver({scaling_in},
            {scaling_out},
            [this, chan, &decim = *decim, &scaling_out = *scaling_out]() {
                // We don't actually change the value here, because the
                // resolution might be not be complete. The resolver for the
                // output scaling can take care of things.
                scaling_out.force_dirty();
            });
        // Resolver for the output scaling: This cannot be updated, we reset it
        // to its previous value.
        add_property_resolver({scaling_out},
            {scaling_out},
            [this,
                chan,
                &decim       = *decim,
                &scaling_in  = *scaling_in,
                &scaling_out = *scaling_out]() {
                scaling_out = scaling_in.get() * _residual_scaling.at(chan);
            });
        // Resolvers for type: These are constants
        add_property_resolver({type_in}, {type_in}, [this, &type_in = *type_in]() {
            type_in.set(IO_TYPE_SC16);
        });
        add_property_resolver({type_out}, {type_out}, [this, &type_out = *type_out]() {
            type_out.set(IO_TYPE_SC16);
        });
    }

    void register_issue_stream_cmd()
    {
        register_action_handler(ACTION_KEY_STREAM_CMD,
            [this](const res_source_info& src, action_info::sptr action) {
                stream_cmd_action_info::sptr stream_cmd_action =
                    std::dynamic_pointer_cast<stream_cmd_action_info>(action);
                if (!stream_cmd_action) {
                    throw uhd::runtime_error(
                        "Received stream_cmd of invalid action type!");
                }
                issue_stream_cmd_action_handler(src, stream_cmd_action);
            });
    }

    void issue_stream_cmd_action_handler(
        const res_source_info& src, stream_cmd_action_info::sptr stream_cmd_action)
    {
        res_source_info dst_edge{
            res_source_info::invert_edge(src.type), src.instance};
        const size_t chan = src.instance;
        uhd::stream_cmd_t::stream_mode_t stream_mode =
            stream_cmd_action->stream_cmd.stream_mode;
        RFNOC_LOG_TRACE("Received stream command: " << char(stream_mode) << " to "
                                                    << src.to_string()
                                                    << ", id==" << stream_cmd_action->id);
        auto new_action = stream_cmd_action_info::make(stream_mode);
        new_action->stream_cmd = stream_cmd_action->stream_cmd;
        if (stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
            || stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE) {
            if (src.type == res_source_info::OUTPUT_EDGE) {
                new_action->stream_cmd.num_samps *= _decim.at(chan).get();
            } else {
                new_action->stream_cmd.num_samps /= _decim.at(chan).get();
            }
            RFNOC_LOG_TRACE("Forwarding num_samps stream command, new value is "
                            << new_action->stream_cmd.num_samps);
        } else {
            RFNOC_LOG_TRACE("Forwarding continuous stream command...")
        }

        post_action(dst_edge, new_action);
    }

    /**************************************************************************
     * FPGA communication (register IO)
     *************************************************************************/
    /*! Update the decimation value
     *
     * \param decim The new decimation value. It must be valid decimation value.
     * \throws uhd::assertion_error if decim is not valid.
     */
    void set_decim(int decim, const size_t chan)
    {
        // Step 1: Calculate number of halfbands
        uint32_t hb_enable = 0;
        uint32_t cic_decim = decim;
        while ((cic_decim % 2 == 0) and hb_enable < _num_halfbands) {
            hb_enable++;
            cic_decim /= 2;
        }
        // Step 2: Make sure we can handle the rest with the CIC
        UHD_ASSERT_THROW(hb_enable <= _num_halfbands);
        UHD_ASSERT_THROW(cic_decim > 0 and cic_decim <= _cic_max_decim);
        const uint32_t decim_word = (hb_enable << 8) | cic_decim;
        regs().poke32(get_addr(SR_DECIM_ADDR, chan), decim_word);

        // Rate change = M/N
        regs().poke32(get_addr(SR_N_ADDR, chan), decim);
        // FIXME:
        // - eiscat DDC had a real mode, where M needed to be 2
        // - TwinRX had some issues with M == 1
        regs().poke32(get_addr(SR_M_ADDR, chan), 1);

        if (cic_decim > 1 and hb_enable == 0) {
            RFNOC_LOG_WARNING(
                "The requested decimation is odd; the user should expect passband "
                "CIC rolloff.\n"
                "Select an even decimation to ensure that a halfband filter is "
                "enabled.\n"
                "Decimations factorable by 4 will enable 2 halfbands, those "
                "factorable by 8 will enable 3 halfbands.\n"
                "decimation = dsp_rate/samp_rate -> "
                << decim);
        }

        constexpr double DDS_GAIN = 2.0;
        // Calculate algorithmic gain of CIC for a given decimation.
        // For Ettus CIC R=decim, M=1, N=4. Gain = (R * M) ^ N
        // The Ettus CIC also tries its best to compensate for the gain by
        // shifting the CIC output. This reduces the gain by a factor of
        // 2**ceil(log2(cic_gain))
        const double cic_gain = std::pow(double(cic_decim * 1), 4);
        // DDS gain:
        const double total_gain =
            DDS_GAIN * cic_gain / std::pow(2, uhd::math::ceil_log2(cic_gain));
        update_scaling(total_gain, chan);
    }

    //! Update scaling based on the current gain
    //
    // Calculates the closest fixpoint value that this block can correct for in
    // hardware (fixpoint). The residual gain is written to _residual_scaling.
    void update_scaling(const double dsp_gain, const size_t chan)
    {
        constexpr double FIXPOINT_SCALING = 1 << 15;
        const double compensation_factor  = 1. / dsp_gain;
        // Convert to fixpoint
        const double target_factor  = FIXPOINT_SCALING * compensation_factor;
        const int32_t actual_factor = boost::math::iround(target_factor);
        // Write DDC with scaling correction for CIC and DDS that maximizes
        // dynamic range
        regs().poke32(get_addr(SR_SCALE_IQ_ADDR, chan), actual_factor);

        // Calculate the error introduced by using fixedpoint representation for
        // the scaler, can be corrected in host later.
        _residual_scaling[chan] = dsp_gain * double(actual_factor) / FIXPOINT_SCALING;
    }

    /*! Return the closest possible decimation value to the one requested
     */
    int coerce_decim(const double requested_decim) const
    {
        UHD_ASSERT_THROW(requested_decim > 0);
        return static_cast<int>(_valid_decims.clip(requested_decim, true));
    }

    //! Set the DDS frequency shift the signal to \p requested_freq
    double _set_freq(
        const double requested_freq, const double input_rate, const size_t chan)
    {
        double actual_freq;
        int32_t freq_word;
        std::tie(actual_freq, freq_word) =
            get_freq_and_freq_word(requested_freq, input_rate);
        regs().poke32(
            get_addr(SR_FREQ_ADDR, chan), uint32_t(freq_word), get_command_time(chan));
        return actual_freq;
    }

    /**************************************************************************
     * Attributes
     *************************************************************************/
    //! Block compat number
    const uint32_t _fpga_compat;
    //! Number of halfbands
    const size_t _num_halfbands;
    //! Max CIC decim
    const size_t _cic_max_decim;

    //! List of valid decimation values
    uhd::meta_range_t _valid_decims;

    //! Cache the current residual scaling
    std::vector<double> _residual_scaling;

    //! Properties for type_in (one per port)
    std::vector<property_t<std::string>> _type_in;
    //! Properties for type_out (one per port)
    std::vector<property_t<std::string>> _type_out;
    //! Properties for samp_rate_in (one per port)
    std::vector<property_t<double>> _samp_rate_in;
    //! Properties for samp_rate_out (one per port)
    std::vector<property_t<double>> _samp_rate_out;
    //! Properties for scaling_in (one per port)
    std::vector<property_t<double>> _scaling_in;
    //! Properties for scaling_out (one per port)
    std::vector<property_t<double>> _scaling_out;
    //! Properties for decim (one per port)
    std::vector<property_t<int>> _decim;
    //! Properties for freq (one per port)
    std::vector<property_t<double>> _freq;
};

UHD_RFNOC_BLOCK_REGISTER_DIRECT(
    ddc_block_control, 0xDDC00000, "DDC", CLOCK_KEY_GRAPH, "bus_clk")
