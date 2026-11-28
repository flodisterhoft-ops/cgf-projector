// stepstool <in.pptx> <out.pptx> <media folder>: see stepstool.pro.
#include "presentationsteps.hpp"
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QTextStream out(stdout), err(stderr);
    if(args.count() != 4)
    {
        err << "usage: stepstool <in.pptx> <out.pptx> <media folder>\n";
        return 2;
    }
    QList<SlideStep> steps;
    QString error;
    if(!PresentationSteps::expand(args.at(1), args.at(2), args.at(3), steps, error))
    {
        err << error << "\n";
        return 1;
    }
    for(const SlideStep &s : std::as_const(steps))
        out << s.toText() << "\n";
    return 0;
}
