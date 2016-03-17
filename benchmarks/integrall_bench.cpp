#include <ctime>
#include <iostream>
#include <cstdlib>
#include <iterator>

#include <integrall.h>
#include <ctime>
#include <limits>
#include <cmath>
#include <chrono>

class Moc_I1:public memseries::statistic::BaseIntegrall{
public:
    Moc_I1(){
        _a=_b=memseries::Meas::empty();
    }
    void calc(const memseries::Meas&a,const memseries::Meas&b)override{
        _a=a;
        _b=b;
    }
    memseries::Meas _a;
    memseries::Meas _b;
};

const size_t K = 1;
void bench_int(memseries::statistic::BaseIntegrall*bi){
    auto m=memseries::Meas::empty();
    for (size_t i = 1; i < K*1000000; i++) {
        m.value=i;
        bi->call(m);
    }
}

int main(int argc, char *argv[]) {
    std::unique_ptr<Moc_I1>  p{new Moc_I1};
    auto start = clock();
    bench_int(p.get());
    auto elapsed = ((float)clock() - start) / CLOCKS_PER_SEC;
    std::cout << "integrall calls: " << elapsed << std::endl;
}
