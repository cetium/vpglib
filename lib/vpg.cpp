/*
 * Copyright (c) 2015, Taranov Alex <pi-null-mezon@yandex.ru>.
 * Released to public domain under terms of the BSD Simplified license.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the organization nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 *   See <http://www.opensource.org/licenses/bsd-license>
 */

#include "vpg.h"
#include <vector>

#ifndef min
#define min(a,b) (((a) > (b))? (b) : (a))
#endif

namespace vpg {

//---------------------------------PulseProcessor--------------------------------
PulseProcessor::PulseProcessor(double dT_ms, ProcessType type)
{
    switch(type){
        case HeartRate:
            __init((int)(7000.0/dT_ms), dT_ms, type);
            break;
        case BreathRate:
        __init((int)(15000.0/dT_ms), dT_ms, type);
            break;
    }
}

PulseProcessor::PulseProcessor(double Tov_ms, double dT_ms, ProcessType type)
{
    __init((int)(Tov_ms/dT_ms), dT_ms, type);
}

void PulseProcessor::__init(int length, double dT_ms, ProcessType type)
{
    m_length = length;
    m_procType = type;
    curpos = 0;    

    switch(m_procType){
        case HeartRate:
            m_Frequency = 80;
            m_interval = 15;
            m_bottomFrequencyLimit = 0.8;
            m_topFrequencyLimit = 3.0;
            break;
        case BreathRate:
            m_Frequency = 16;
            m_bottomFrequencyLimit = 0.2;
            m_topFrequencyLimit = 0.7;
            break;
    }

    v_raw = new double[m_length];
    v_Y = new double[m_length];
    v_time = new double[m_length];
    v_FA = new double[m_length/2 + 1];

    for(int i = 0; i < m_length; i++)  {
        v_raw[i] = (double)(i % 2);
        v_Y[i] = 0;
        v_time[i] = dT_ms;
    }
    v_X = new double[3];
	for(int i = 0; i < 3; i ++) 
		v_X[i] = (double)i;

    v_data = new cv::Mat(1, m_length, CV_64F);
    v_dft = new cv::Mat(1, m_length, CV_64F);     
}

PulseProcessor::~PulseProcessor()
{
    delete[] v_raw;
    delete[] v_Y;
    delete[] v_X;
    delete[] v_time;
    delete[] v_FA;
    delete v_dft;
    delete v_data;
}

void PulseProcessor::update(double value, double time)
{
    v_raw[curpos] = value;
    v_time[curpos] = time;

    double mean = 0.0;
    double sko = 0.0;

    for(int i = 0; i < m_interval; i++) {
        mean += v_raw[__loop(curpos - i)];
    }
    mean /= m_interval;
    int pos = 0;
    for(int i = 0; i < m_interval; i++) {
        pos = __loop(curpos - i);
        sko += (v_raw[pos] - mean)*(v_raw[pos] - mean);
    }
    sko = std::sqrt( sko/(m_interval - 1));
    if(sko < 0.01) {
        sko = 1.0;
    }
    v_X[__seek(curpos)] = (v_raw[curpos] - mean)/ sko;
    v_Y[curpos] = ( v_X[__seek(curpos)] + v_X[__seek(curpos - 1)] + v_X[__seek(curpos - 2)] + v_Y[__loop(curpos - 1)] ) / 4.0;

    curpos = (++curpos) % m_length;
}

int PulseProcessor::computeHR()
{
    double time = 0.0;
    for (int i = 0; i < m_length; i++) {
        time += v_time[i];
    }
    double *pt = v_data->ptr<double>(0);
    for(int i = 0; i < m_length; i++)
        pt[i] = v_Y[__loop(curpos - 1 - i)];
    cv::dft(*v_data, *v_dft);
    const double *v_fft = v_dft->ptr<const double>(0);

    // complex-conjugate-symmetrical array
    v_FA[0] = v_fft[0]*v_fft[0];
    if((m_length % 2) == 0) { // Even number of counts
        for(int i = 1; i < m_length/2; i++)
            v_FA[i] = v_fft[2*i-1]*v_fft[2*i-1] + v_fft[2*i]*v_fft[2*i];
        v_FA[m_length/2] = v_fft[m_length-1]*v_fft[m_length-1];
    } else { // Odd number of counts
        for(int i = 1; i <= m_length/2; i++)
            v_FA[i] = v_fft[2*i-1]*v_fft[2*i-1] + v_fft[2*i]*v_fft[2*i];
    }

    int bottom = (int)(m_bottomFrequencyLimit * time / 1000.0);
    int top = (int)(m_topFrequencyLimit * time / 1000.0);
    if(top > (m_length/2))
        top = m_length/2;
    int i_maxpower = 0;
    double maxpower = 0.0;
    for (int i = bottom + 2 ; i <= top - 2; i++)
        if ( maxpower < v_FA[i] ) {
            maxpower = v_FA[i];
            i_maxpower = i;
        }

    double noise_power = 0.0;
    double signal_power = 0.0;
    double signal_moment = 0.0;
    for (int i = bottom; i <= top; i++)    {
        if ( (i >= i_maxpower - 2) && (i <= i_maxpower + 2) )       {
            signal_power += v_FA[i];
            signal_moment += i * v_FA[i];
        } else {
            noise_power += v_FA[i];
        }
    }

    double snr = 0.0;
    if(signal_power > 0.01) {
        snr = 10.0 * std::log10( signal_power / noise_power );
        double bias = (double)i_maxpower - ( signal_moment / signal_power );
        snr *= (1.0 / (1.0 + bias*bias));
    }
    if(snr > 2.0)
        m_Frequency = (signal_moment / signal_power) * 60000.0 / time;

    if(m_Frequency > 150.0)
        m_interval = 11;
    else if(m_Frequency > 110.0)
        m_interval = 13;
    else if(m_Frequency > 70.0)
        m_interval = 15;
    else m_interval = 17;

    return (int)m_Frequency;
}

int PulseProcessor::getLength() const
{
    return m_length;
}

const double * PulseProcessor::getSignal() const
{
    return v_Y;
}


int PulseProcessor::__loop(int d) const
{
    return ((m_length + (d % m_length)) % m_length);
}

int PulseProcessor::__seek(int d) const
{
    return ((3 + (d % 3)) % 3);
}

//--------------------------------FaceProcessor--------------------------------

#define FACE_PROCESSOR_LENGTH 16


FaceProcessor::FaceProcessor(const std::string &filename)
{
    __init();
    loadClassifier(filename);
}

FaceProcessor::FaceProcessor()
{
    __init();
}

void FaceProcessor::__init()
{
    v_rects = new cv::Rect[FACE_PROCESSOR_LENGTH];
    m_pos = 0;
    m_nofaceframes = 0;
    f_firstface = true;    
}

FaceProcessor::~FaceProcessor()
{
    delete[] v_rects;
}

void FaceProcessor::enrollImage(const cv::Mat &rgb, double &resV, double &resT)
{
    if(f_firstface)
        m_markTime = cv::getTickCount();
    cv::Mat gray;
    cv::cvtColor(rgb, gray, CV_BGR2GRAY);
    cv::equalizeHist(gray, gray);
    std::vector<cv::Rect> faces;
    m_classifier.detectMultiScale(gray, faces, 1.1, 5,
                                  cv::CASCADE_FIND_BIGGEST_OBJECT,
                                  cv::Size(80, 100));
    if(faces.size() > 0) {
        __updateRects(faces[0]);
        m_nofaceframes = 0;
        f_firstface = false;
    } else {
        m_nofaceframes++;
        if(m_nofaceframes == FACE_PROCESSOR_LENGTH) {
            f_firstface = true;
            __updateRects(cv::Rect(0,0,0,0));
        }
    }
    cv::Rect faceRect = getFaceRect() & cv::Rect(0,0,rgb.cols, rgb.rows);

    unsigned int W = faceRect.width;
    unsigned int H = faceRect.height;
    unsigned int dX = W/16;
    unsigned int dY = H/30;
    unsigned long green = 0;
    unsigned long area = 0;

    if(faceRect.area() > 0 && m_nofaceframes < FACE_PROCESSOR_LENGTH) {
            cv::Mat region = cv::Mat(rgb, faceRect).clone();
            cv::blur(region,region, cv::Size(4,4));
            m_ellRect = cv::Rect(dX, -6 * dY, W - 2 * dX, H + 6 * dY);
            uint X = m_ellRect.x;
            W = m_ellRect.width;
            unsigned char *p;
            unsigned char tR = 0, tG = 0, tB = 0;
            for(unsigned int j = 0; j < H; j++) {
                p = region.ptr(j);
                for(unsigned int i = X; i < X + W; i++) {
                    tB = p[3*i];
                    tG = p[3*i+1];
                    tR = p[3*i+2];
                    if( __skinColor(tR, tG, tB) && __insideEllipse(i, j)) {
                        area++;
                        green += tG;
                    }
                }
            }
        }

    resT = ((double)cv::getTickCount() -  (double)m_markTime)*1000.0 / cv::getTickFrequency();
    m_markTime = cv::getTickCount();
    if(area > 5000)
        resV = (double)green / area;
    else
        resV = 0.0;
}

cv::Rect FaceProcessor::getFaceRect() const
{
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    for(int i = 0; i < FACE_PROCESSOR_LENGTH; i++) {
        x += v_rects[i].x;
        y += v_rects[i].y;
        w += v_rects[i].width;
        h += v_rects[i].height;
    }
    x /= FACE_PROCESSOR_LENGTH;
    y /= FACE_PROCESSOR_LENGTH;
    w /= FACE_PROCESSOR_LENGTH;
    h /= FACE_PROCESSOR_LENGTH;
    return cv::Rect((int)x, (int)y, (int)w, (int)h);
}


bool FaceProcessor::loadClassifier(const std::string &filename)
{
    return m_classifier.load(filename);
}

void FaceProcessor::__updateRects(const cv::Rect &rect)
{
    if(f_firstface == false){
        v_rects[m_pos] = rect;
        m_pos = (++m_pos) % FACE_PROCESSOR_LENGTH;
    } else {
        for(int i = 0; i < FACE_PROCESSOR_LENGTH; i++)
            v_rects[i] = rect;
    }
}

bool FaceProcessor::__insideEllipse(int x, int y) const
{
    double cx = (m_ellRect.x + m_ellRect.width / 2.0 - x) / (m_ellRect.width / 2.0);
    double cy = (m_ellRect.y + m_ellRect.height / 2.0 - y) / (m_ellRect.height / 2.0);
    if( (cx*cx + cy*cy) < 1.0 )
        return true;
    else
        return false;
}

bool FaceProcessor::__skinColor(unsigned char vR, unsigned char vG, unsigned char vB) const
{
    if( (vR > 95) && (vR > vG) && (vG > 40) && (vB > 20) && ((vR - min(vG,vB)) > 7) && ((vR - vG) > 7) )
        return true;
    else
        return false;
}
//------------------------------End of FaceProcessor--------------------------------
} // end of namespace vpg