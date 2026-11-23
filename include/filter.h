#ifndef ATG_ENGINE_SIM_FILTER_H
#define ATG_ENGINE_SIM_FILTER_H

class Filter {
    public:
        Filter() = default;
        virtual ~Filter() = default;

        virtual float f(float sample) { return sample; }
        virtual void destroy() {}
};

#endif /* ATG_ENGINE_SIM_FILTER_H */
